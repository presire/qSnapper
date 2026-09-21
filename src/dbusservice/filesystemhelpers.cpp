#include <QByteArray>
#include <QStringList>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include "filesystemhelpers.h"

namespace qsnapper::security {
    namespace {
        /**
         * @brief 絶対パスを各成分へ分解する
         *
         * @param path 分解対象の絶対パス
         * @param components 分解結果の格納先
         * @return 絶対パスとして分解できた場合: true
         */
        bool splitAbsolutePath(const QString &path, QStringList *components)
        {
            if (!components) {
                errno = EINVAL;
                return false;
            }

            // 呼び出し側から渡された出力先は毎回クリアして使用する
            components->clear();
            if (!path.startsWith(QLatin1Char('/'))) {
                errno = EINVAL;
                return false;
            }

            *components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            return true;
        }

        /**
         * @brief close()漏れを防ぐためのfd所有ガード (RAII)
         *
         * コピー禁止
         * エラー路径を含めて全てのfd解放を保証する
         */
        class UniqueFd {
        public:
            explicit UniqueFd(int fd = -1)
                : m_fd(fd)
            {
            }

            ~UniqueFd()
            {
                reset();
            }

            UniqueFd(const UniqueFd &) = delete;
            UniqueFd &operator=(const UniqueFd &) = delete;

            int get() const
            {
                return m_fd;
            }

            bool isValid() const
            {
                return m_fd >= 0;
            }

            void reset()
            {
                if (m_fd >= 0) {
                    ::close(m_fd);
                    m_fd = -1;
                }
            }

        private:
            int m_fd;
        };

        /**
         * @brief 制御文字 (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含むか判定する
         *
         * 埋め込みNULはsyscall引数の切り詰めを招き、検証したパスと実際に変異するパスが
         * ズレる原因となるため、パスには一切許容しない (inputvalidatorと同じ方針)
         */
        bool containsControlChar(const QString &value)
        {
            for (const QChar &c : value) {
                const ushort u = c.unicode();
                if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief dirfd相対パスを検証してUTF-8へ符号化する
         *
         * 空パス・絶対パス・"." / ".." 成分・制御文字を拒否する
         * openat系は絶対パスを渡すとdirfdを無視するため、pin済みdirfdから意図しない位置へ解決されるのを入力段階で防ぐ
         *
         * @param relativePath 検証対象の相対パス
         * @param encodedOut 符号化結果の格納先 (省略可)
         * @return 有効な相対パスの場合: true
         */
        bool validateRelativePathAt(const QString &relativePath, QByteArray *encodedOut)
        {
            if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/'))
                    || containsControlChar(relativePath)) {
                errno = EINVAL;
                return false;
            }

            const QStringList components =
                    relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (components.isEmpty()) {
                errno = EINVAL;
                return false;
            }
            for (const QString &component : components) {
                if (component == QLatin1String(".") || component == QLatin1String("..")) {
                    errno = EINVAL;
                    return false;
                }
            }

            if (encodedOut) {
                *encodedOut = relativePath.toUtf8();
            }
            return true;
        }

        /**
         * @brief ルートディレクトリ "/" を開く
         *
         * @return 成功時: dirfd、失敗時: -1
         */
        int openRootDirectory()
        {
            return ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }

        /**
         * @brief 基点ディレクトリrootPathを全成分O_NOFOLLOWで開く
         *
         * 単純なopen(rootPath, O_NOFOLLOW)は末尾成分にしかO_NOFOLLOWが効かず、rootPathの中間成分がsymlink化されていた場合に追従してしまう
         * そのため既存のsafeOpenDirectory() (「/」起点の成分単位no-follow walk) へ委譲し、root祖先の差し替えも拒否する
         *
         * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
         * @return 成功時: dirfd、失敗時: -1
         */
        int openRootPathDirectory(const QString &rootPath)
        {
            if (rootPath.isEmpty() || !rootPath.startsWith(QLatin1Char('/'))
                    || containsControlChar(rootPath)) {
                errno = EINVAL;
                return -1;
            }

            return safeOpenDirectory(rootPath);
        }

        /**
         * @brief baseFd配下の成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く
         *
         * baseFdの所有は取らない (openat(baseFd, ".") で同一ディレクトリの所有fdを得る)
         * 成功時は呼び出し側がcloseすべきleaf fd、失敗時は-1を返す
         *
         * @param baseFd 走査基点のディレクトリfd
         * @param components 相対パス成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分をmkdirat()で作成するか
         * @param mode createMissing = true時の作成モード
         * @return 成功時: 最終ディレクトリのfd、失敗時: -1
         */
        int openDirectoryChainAtNoFollow(int baseFd, const QStringList &components,
                                         int componentCount, bool createMissing, mode_t mode)
        {
            int dirFd = ::openat(baseFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dirFd < 0) {
                return -1;
            }

            for (int i = 0; i < componentCount; ++i) {
                const QByteArray encodedName = components.at(i).toUtf8();
                if (createMissing) {
                    // 中間成分が無ければその場で作成し、直後にO_NOFOLLOWで開き直す
                    if (::mkdirat(dirFd, encodedName.constData(), mode) < 0 && errno != EEXIST) {
                        const int savedErrno = errno;
                        ::close(dirFd);
                        errno = savedErrno;
                        return -1;
                    }
                }

                // 各成分をO_NOFOLLOW付きで開くことで、中間symlinkを拒否する
                int nextFd = ::openat(dirFd, encodedName.constData(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (nextFd < 0) {
                    const int savedErrno = errno;
                    ::close(dirFd);
                    errno = savedErrno;
                    return -1;
                }

                ::close(dirFd);
                dirFd = nextFd;
            }

            return dirFd;
        }

        int openDirectoryChainAtOpenat2(int baseFd, const QStringList &components,
                                        int componentCount, bool createMissing, mode_t mode)
        {
            struct open_how how = {};
            how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
            how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;

            auto openBeneathBase = [&how, baseFd](const QByteArray &relativePath) {
                constexpr int MAX_EAGAIN_RETRIES = 3;
                for (int attempt = 0; attempt < MAX_EAGAIN_RETRIES; ++attempt) {
                    const long result = ::syscall(SYS_openat2, baseFd, relativePath.constData(),
                                                  &how, sizeof(how));
                    if (result >= 0) {
                        return static_cast<int>(result);
                    }
                    if (errno != EAGAIN || attempt == MAX_EAGAIN_RETRIES - 1) {
                        return -1;
                    }
                }
                return -1;
            };

            int dirFd = openBeneathBase(QByteArrayLiteral("."));
            if (dirFd < 0) {
                return -1;
            }

            QByteArray relativePath;
            for (int i = 0; i < componentCount; ++i) {
                const QByteArray encodedName = components.at(i).toUtf8();
                if (createMissing) {
                    if (::mkdirat(dirFd, encodedName.constData(), mode) < 0 && errno != EEXIST) {
                        const int savedErrno = errno;
                        ::close(dirFd);
                        errno = savedErrno;
                        return -1;
                    }
                }

                if (!relativePath.isEmpty()) {
                    relativePath.append('/');
                }
                relativePath.append(encodedName);

                const int nextFd = openBeneathBase(relativePath);
                if (nextFd < 0) {
                    const int savedErrno = errno;
                    ::close(dirFd);
                    errno = savedErrno;
                    return -1;
                }

                ::close(dirFd);
                dirFd = nextFd;
            }

            return dirFd;
        }

        int openDirectoryChainAt(int baseFd, const QStringList &components, int componentCount,
                                 bool createMissing, mode_t mode)
        {
            const int dirFd = openDirectoryChainAtOpenat2(baseFd, components, componentCount,
                                                           createMissing, mode);
            if (dirFd >= 0 || errno != ENOSYS) {
                return dirFd;
            }

            return openDirectoryChainAtNoFollow(baseFd, components, componentCount,
                                                createMissing, mode);
        }

        /**
         * @brief パス成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く ("/"起点)
         *
         * @param components 絶対パスを分解した成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分を mkdirat() で作成するか
         * @param mode createMissing = true時の作成モード
         * @return 成功時: 最終ディレクトリのfd、失敗時: -1
         */
        int openDirectoryChainNoFollow(const QStringList &components, int componentCount,
                                       bool createMissing, mode_t mode)
        {
            const int rootFd = openRootDirectory();
            if (rootFd < 0) {
                return -1;
            }

            const int leafFd = openDirectoryChainAt(rootFd, components, componentCount,
                                                    createMissing, mode);
            const int savedErrno = errno;
            ::close(rootFd);
            errno = savedErrno;
            return leafFd;
        }

        /**
         * @brief 宛先絶対パスをrootPath配下の相対成分列へ分解する (内部用)
         *
         * @param rootPath 基点ルートディレクトリ
         * @param absolutePath 宛先の絶対パス
         * @param components 相対成分列の格納先 (非空であることを保証する)
         * @return 分解できた場合: true
         */
        bool splitDestinationComponents(const QString &rootPath, const QString &absolutePath,
                                        QStringList *components)
        {
            QString relative;
            if (!splitDestinationBeneathRoot(rootPath, absolutePath, &relative)) {
                return false;
            }

            *components = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (components->isEmpty()) {
                errno = EINVAL;
                return false;
            }
            return true;
        }

        /**
         * @brief rootPathを開き、相対成分列のleaf「親」までO_NOFOLLOWで辿る
         *
         * root自身はO_NOFOLLOWで開くため、rootがsymlinkへ差し替えられている場合も拒否する
         * 中間成分は openat(..., O_DIRECTORY | O_NOFOLLOW) の連鎖で解決するため、どの階層のsymlink差し替えでもELOOPで失敗する
         *
         * @param rootPath 基点ルートディレクトリ
         * @param relativeComponents splitDestinationComponents由来の相対成分列 (非空)
         * @param createMissing 中間成分を作成するか
         * @param mode createMissing = true時の作成モード
         * @param leafName 最終成分名の返却先
         * @return 成功時: 親dirfd (呼び出し側でclose)、失敗時: -1
         */
        int openLeafParentBeneathRoot(const QString &rootPath,
                                      const QStringList &relativeComponents,
                                      bool createMissing, mode_t mode, QByteArray *leafName)
        {
            if (relativeComponents.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafName) {
                *leafName = relativeComponents.constLast().toUtf8();
            }

            const UniqueFd rootFd(openRootPathDirectory(rootPath));
            if (!rootFd.isValid()) {
                return -1;
            }

            return openDirectoryChainAt(rootFd.get(), relativeComponents,
                                        static_cast<int>(relativeComponents.size()) - 1,
                                        createMissing, mode);
        }

        /**
         * @brief パスの親ディレクトリをO_NOFOLLOWで開き、leaf名も返す
         *
         * @param path 対象パス
         * @param createMissing 親ディレクトリが無い場合に作成するか
         * @param mode createMissing = true時の作成モード
         * @param leafName 最終要素の返却先
         * @return 成功時: 親ディレクトリfd、失敗時: -1
         */
        int openParentDirectoryNoFollow(const QString &path, bool createMissing, mode_t mode,
                                        QByteArray *leafName)
        {
            QStringList components;
            if (!splitAbsolutePath(path, &components) || components.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafName) {
                *leafName = components.constLast().toUtf8();
            }

            return openDirectoryChainNoFollow(components, components.size() - 1, createMissing, mode);
        }

        /**
         * @brief 親dirfd配下の1エントリをsymlink非追従で削除する
         *
         * @param parentFd 親ディレクトリfd
         * @param entryName 削除対象のleaf名
         * @return 削除成功時: true
         */
        bool removeEntryAt(int parentFd, const QByteArray &entryName)
        {
            struct stat st;
            if (::fstatat(parentFd, entryName.constData(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
                return errno == ENOENT;
            }

            // symlink / regular file / fifo / deviceは、unlinkat()側で削除する
            if (!S_ISDIR(st.st_mode)) {
                return ::unlinkat(parentFd, entryName.constData(), 0) == 0 || errno == ENOENT;
            }

            int childFd = ::openat(parentFd, entryName.constData(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (childFd < 0) {
                return false;
            }

            DIR *dir = ::fdopendir(childFd);
            if (!dir) {
                const int savedErrno = errno;
                ::close(childFd);
                errno = savedErrno;
                return false;
            }

            while (dirent *entry = ::readdir(dir)) {
                // "." と ".." は再帰対象から除外する
                const char *name = entry->d_name;
                if ((name[0] == '.' && name[1] == '\0')
                        || (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
                    continue;
                }

                if (!removeEntryAt(childFd, QByteArray(name))) {
                    const int savedErrno = errno;
                    ::closedir(dir);
                    errno = savedErrno;
                    return false;
                }
            }

            if (::closedir(dir) < 0) {
                return false;
            }

            return ::unlinkat(parentFd, entryName.constData(), AT_REMOVEDIR) == 0 || errno == ENOENT;
        }

        /**
         * @brief 開いたfdがregular fileかを検証する
         *
         * @param fd 検証対象fd
         * @return regular fileの場合: true
         */
        bool validateRegularFileDescriptor(int fd)
        {
            struct stat st;
            if (::fstat(fd, &st) < 0) {
                return false;
            }

            if (!S_ISREG(st.st_mode)) {
                errno = EINVAL;
                return false;
            }

            return true;
        }

        /**
         * @brief dirfd基点で親成分列をopenat2(RESOLVE_IN_ROOT)で解決する (内部用)
         *
         * snapshot dirfdは"/"のbtrfs snapshotであるため、"var/run -> /run" のような
         * 絶対symlinkはsnapshot内で正当な存在であり、chrootと同じくroot内解決が要求される
         * RESOLVE_IN_ROOTはカーネル内でroot外への逸脱を構造的に不可能にする
         * RESOLVE_NO_MAGICLINKSにより/proc系のmagic linkも追従しない
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param parentComponents leaf成分を除いた親成分列 (非空)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootAtOpenat2(int dirFd, const QStringList &parentComponents)
        {
            struct open_how how = {};
            how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
            how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;

            const QByteArray encodedParent =
                    parentComponents.join(QLatin1Char('/')).toUtf8();

            constexpr int MAX_EAGAIN_RETRIES = 3;
            for (int attempt = 0; attempt < MAX_EAGAIN_RETRIES; ++attempt) {
                const long result = ::syscall(SYS_openat2, dirFd, encodedParent.constData(),
                                              &how, sizeof(how));
                if (result >= 0) {
                    return static_cast<int>(result);
                }
                if (errno != EAGAIN || attempt == MAX_EAGAIN_RETRIES - 1) {
                    return -1;
                }
            }
            return -1;
        }

        /**
         * @brief dirfd基点で親成分列を手動のin-root解決で辿る (openat2非対応カーネル用フォールバック)
         *
         * RESOLVE_IN_ROOT相当の解決をfdスタックで再現する:
         *  - 各成分はfstatat(AT_SYMLINK_NOFOLLOW)で検査してからopenat(O_NOFOLLOW)で開く
         *  - symlinkのtargetはroot内で解釈し、絶対targetならrootへ巻き戻す
         *  - ".." はroot自身ではrootに留まる ("/.." が"/"であるのと同じ挙動)
         * このため絶対symlink targetがdirfdの外へ解決されることは構造的にない
         *
         * symlink targetは生バイト列であり、非UTF-8のファイル名を含み得る
         * work listをQByteArrayで保持することで、検証済み成分もtarget成分もバイト単位で扱う
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param parentComponents leaf成分を除いた親成分列 (非空)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootAtNoFollow(int dirFd, const QStringList &parentComponents)
        {
            // symlink連鎖とwork listの無制限成長に対する上限 (カーネルの40回相当)
            constexpr int MAX_SYMLINK_FOLLOWS = 40;
            constexpr int MAX_PROCESSED_COMPONENTS = 4096;

            const int rootFd = ::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (rootFd < 0) {
                return -1;
            }

            // stack[0]はrootであり、決してpopしない
            QList<int> stack;
            stack.append(rootFd);

            auto closeStack = [&stack]() {
                for (int fd : stack) {
                    ::close(fd);
                }
                stack.clear();
            };

            int symlinkBudget = MAX_SYMLINK_FOLLOWS;
            int processedCount = 0;
            QList<QByteArray> workList;
            for (const QString &component : parentComponents) {
                workList.append(component.toUtf8());
            }

            while (!workList.isEmpty()) {
                if (++processedCount > MAX_PROCESSED_COMPONENTS) {
                    closeStack();
                    errno = ELOOP;
                    return -1;
                }

                const QByteArray component = workList.takeFirst();
                if (component == ".") {
                    continue;
                }
                if (component == "..") {
                    // RESOLVE_IN_ROOT相当: root自身の".."はrootに留まる
                    if (stack.size() > 1) {
                        ::close(stack.takeLast());
                    }
                    continue;
                }

                struct stat st;
                if (::fstatat(stack.last(), component.constData(), &st,
                              AT_SYMLINK_NOFOLLOW) < 0) {
                    const int savedErrno = errno;
                    closeStack();
                    errno = savedErrno;
                    return -1;
                }

                if (S_ISLNK(st.st_mode)) {
                    if (--symlinkBudget < 0) {
                        closeStack();
                        errno = ELOOP;
                        return -1;
                    }

                    // 固定PATH_MAXで切り詰めないよう、必要に応じてバッファを拡張する
                    QByteArray buffer(256, '\0');
                    for (;;) {
                        const ssize_t len = ::readlinkat(stack.last(), component.constData(),
                                                         buffer.data(),
                                                         static_cast<size_t>(buffer.size()));
                        if (len < 0) {
                            const int savedErrno = errno;
                            closeStack();
                            errno = savedErrno;
                            return -1;
                        }
                        if (len < buffer.size()) {
                            buffer.truncate(static_cast<qsizetype>(len));
                            break;
                        }
                        buffer.resize(buffer.size() * 2);
                    }

                    // 空targetはカーネルの解決でもENOENTとなる
                    if (buffer.isEmpty()) {
                        closeStack();
                        errno = ENOENT;
                        return -1;
                    }

                    // 絶対targetはdirfdの外ではなくrootへ巻き戻す (RESOLVE_IN_ROOTの解決)
                    if (buffer.startsWith('/')) {
                        while (stack.size() > 1) {
                            ::close(stack.takeLast());
                        }
                    }

                    // target成分を解決済みパスの先頭へ挿入する ("//" は単一の区切りとして扱う)
                    const QList<QByteArray> targetComponents =
                            buffer.split('/');
                    for (int i = targetComponents.size() - 1; i >= 0; --i) {
                        if (!targetComponents.at(i).isEmpty()) {
                            workList.prepend(targetComponents.at(i));
                        }
                    }
                    continue;
                }

                const int nextFd = ::openat(stack.last(), component.constData(),
                                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (nextFd < 0) {
                    const int savedErrno = errno;
                    closeStack();
                    errno = savedErrno;
                    return -1;
                }
                stack.append(nextFd);
            }

            // 成功: 最上位fdの所有を呼び出し側へ移し、残りを全てcloseする
            const int resultFd = stack.takeLast();
            closeStack();
            return resultFd;
        }

        /**
         * @brief dirfd基点で親ディレクトリをin-root解決により開き、leaf名も返す
         *
         * 検証済みrelativePathと同一の入力から親dirfdとleaf名を導出することで、
         * 「検証した値」と「操作する対象」の一致 (TOCTOUなし) を保証する
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param components 検証済み相対パスの成分列 (非空)
         * @param leafNameOut 最終成分名の返却先 (省略可)
         * @param forceNoOpenat2 openat2を使わずフォールバック経路を強制するか (テスト用)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootAt(int dirFd, const QStringList &components,
                                        QByteArray *leafNameOut,
                                        bool forceNoOpenat2 = false)
        {
            if (dirFd < 0 || components.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafNameOut) {
                *leafNameOut = components.constLast().toUtf8();
            }

            // leafが1成分のみの場合、親はdirfd自身
            // 呼び出し側が常にcloseできるように、所有fdを複製して返す
            if (components.size() == 1) {
                return ::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            }

            const QStringList parentComponents =
                    components.mid(0, components.size() - 1);

            if (!forceNoOpenat2) {
                const int parentFd =
                        openParentDirectoryInRootAtOpenat2(dirFd, parentComponents);
                if (parentFd >= 0 || errno != ENOSYS) {
                    return parentFd;
                }
            }

            return openParentDirectoryInRootAtNoFollow(dirFd, parentComponents);
        }
    } // namespace

    /**
     * @brief シンボリックリンクを辿らずにディレクトリ階層を作成する
     *
     * @param path 作成対象の絶対パス
     * @param mode 新規作成ディレクトリのモード
     * @return 成功時: true
     */
    bool safeMkpath(const QString &path, mode_t mode)
    {
        if (path.isEmpty() || path == QStringLiteral("/")) {
            return true;
        }

        QStringList components;
        if (!splitAbsolutePath(path, &components)) {
            return false;
        }

        int dirFd = openDirectoryChainNoFollow(components, components.size(), true, mode);
        if (dirFd < 0) {
            return false;
        }

        ::close(dirFd);
        return true;
    }

    /**
     * @brief 既存ディレクトリをO_NOFOLLOWで開く
     *
     * @param path 対象ディレクトリの絶対パス
     * @return 成功時: dirfd、失敗時: -1
     */
    int safeOpenDirectory(const QString &path)
    {
        if (path.isEmpty() || path == QStringLiteral("/")) {
            return openRootDirectory();
        }

        QStringList components;
        if (!splitAbsolutePath(path, &components)) {
            return -1;
        }

        return openDirectoryChainNoFollow(components, components.size(), false, 0);
    }

    /**
     * @brief 通常ファイルを安全に書き込みオープンする
     *
     * @param path 対象ファイルの絶対パス
     * @param mode 作成時モード
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileWrite(const QString &path, mode_t mode)
    {
        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, mode, &leafName);
        if (parentFd < 0) {
            return -1;
        }

        // 親ディレクトリfdを固定したopenat()により、leafを作成 / 上書きする
        const int fd = ::openat(parentFd, leafName.constData(),
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                                mode);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        if (fd < 0) {
            return -1;
        }

        if (!validateRegularFileDescriptor(fd)) {
            const int validationErrno = errno;
            ::close(fd);
            errno = validationErrno;
            return -1;
        }

        return fd;
    }

    /**
     * @brief 通常ファイルを安全に読み取りオープンする
     *
     * @param path 対象ファイルの絶対パス
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileRead(const QString &path)
    {
        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, 0, &leafName);
        if (parentFd < 0) {
            return -1;
        }

        const int fd = ::openat(parentFd, leafName.constData(),
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        if (fd < 0) {
            return -1;
        }

        if (!validateRegularFileDescriptor(fd)) {
            const int validationErrno = errno;
            ::close(fd);
            errno = validationErrno;
            return -1;
        }

        return fd;
    }

    /**
     * @brief パス配下をsymlink非追従で再帰削除する
     *
     * @param path 削除対象の絶対パス
     * @return 削除成功時: true
     */
    bool safeRemoveAll(const QString &path)
    {
        if (path.isEmpty() || path == QStringLiteral("/")) {
            errno = EINVAL;
            return false;
        }

        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, 0, &leafName);
        if (parentFd < 0) {
            return errno == ENOENT;
        }

        const bool removed = removeEntryAt(parentFd, leafName);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        return removed;
    }

    /**
     * @brief 親dirfdを固定したrenameat()でパスを移動する
     *
     * @param sourcePath 移動元の絶対パス
     * @param destinationPath 移動先の絶対パス
     * @return 成功時: true
     */
    bool safeRenamePathNoFollow(const QString &sourcePath, const QString &destinationPath)
    {
        QByteArray sourceLeaf;
        const int sourceParentFd = openParentDirectoryNoFollow(sourcePath, false, 0, &sourceLeaf);
        if (sourceParentFd < 0) {
            return false;
        }

        QByteArray destinationLeaf;
        const int destinationParentFd = openParentDirectoryNoFollow(destinationPath, false, 0, &destinationLeaf);
        if (destinationParentFd < 0) {
            const int savedErrno = errno;
            ::close(sourceParentFd);
            errno = savedErrno;
            return false;
        }

        const bool ok = (::renameat(sourceParentFd, sourceLeaf.constData(),
                                    destinationParentFd, destinationLeaf.constData()) == 0);
        const int savedErrno = errno;
        ::close(destinationParentFd);
        ::close(sourceParentFd);
        errno = savedErrno;
        return ok;
    }

    /**
     * @brief symlink targetをreadlinkat()で取得する
     *
     * @param path 対象symlinkの絶対パス
     * @param targetOut 読み出したtargetの格納先
     * @return 成功時: true
     */
    bool safeReadLinkNoFollow(const QString &path, QByteArray *targetOut)
    {
        if (!targetOut) {
            errno = EINVAL;
            return false;
        }

        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, 0, &leafName);
        if (parentFd < 0) {
            return false;
        }

        // 固定PATH_MAXで切り詰めないよう、必要に応じてバッファを拡張する
        QByteArray buffer(256, '\0');
        for (;;) {
            const ssize_t len = ::readlinkat(parentFd, leafName.constData(), buffer.data(),
                                             static_cast<size_t>(buffer.size()));
            if (len < 0) {
                const int savedErrno = errno;
                ::close(parentFd);
                errno = savedErrno;
                return false;
            }

            if (len < buffer.size()) {
                buffer.truncate(static_cast<qsizetype>(len));
                *targetOut = buffer;
                ::close(parentFd);
                return true;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    /**
     * @brief symlinkat()を用いてsymlinkを作成する
     *
     * @param target 作成するsymlinkのtarget
     * @param path 作成先の絶対パス
     * @return 成功時: true
     */
    bool safeCreateSymlinkNoFollow(const QByteArray &target, const QString &path)
    {
        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, 0, &leafName);
        if (parentFd < 0) {
            return false;
        }

        const bool ok = (::symlinkat(target.constData(), parentFd, leafName.constData()) == 0);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        return ok;
    }

    /**
     * @brief symlink自体のowner / timesをAT_SYMLINK_NOFOLLOWで更新する
     *
     * @param path 対象symlinkの絶対パス
     * @param owner 設定するowner UID
     * @param group 設定するgroup GID
     * @param times 設定するaccess / modification time
     * @param ownerUpdated owner更新成功結果の返却先
     * @param timesUpdated times更新成功結果の返却先
     * @return 両方成功した場合 true
     */
    bool safeSetSymlinkMetadataNoFollow(const QString &path,
                                        uid_t owner,
                                        gid_t group,
                                        const struct timespec times[2],
                                        bool *ownerUpdated,
                                        bool *timesUpdated)
    {
        if (!ownerUpdated || !timesUpdated || !times) {
            errno = EINVAL;
            return false;
        }

        *ownerUpdated = false;
        *timesUpdated = false;

        QByteArray leafName;
        const int parentFd = openParentDirectoryNoFollow(path, false, 0, &leafName);
        if (parentFd < 0) {
            return false;
        }

        bool allOk = true;
        if (::fchownat(parentFd, leafName.constData(), owner, group, AT_SYMLINK_NOFOLLOW) == 0) {
            *ownerUpdated = true;
        }
        else {
            allOk = false;
        }

        if (::utimensat(parentFd, leafName.constData(), times, AT_SYMLINK_NOFOLLOW) == 0) {
            *timesUpdated = true;
        }
        else {
            allOk = false;
        }

        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        return allOk;
    }

    /**
     * @brief read-only用途でlstat()を行う
     *
     * @param path 対象パス
     * @param out stat 構造体の出力先
     * @return 成功時: true
     */
    bool safeLstat(const QString &path, struct stat *out)
    {
        if (!out) {
            errno = EINVAL;
            return false;
        }

        // 契約および使用上の制約は、filesystemhelpers.hを参照すること
        const QByteArray encodedPath = path.toUtf8();
        return ::lstat(encodedPath.constData(), out) == 0;
    }

    bool safeLstatAt(int dirFd, const QString &relativePath, struct stat *out)
    {
        if (!out || dirFd < 0) {
            errno = EINVAL;
            return false;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return false;
        }

        // 検証済みの同一relativePathから親dirfdとleaf名を導出する (TOCTOUなし)
        const QStringList components =
                relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QByteArray leafName;
        const int parentFd = openParentDirectoryInRootAt(dirFd, components, &leafName);
        if (parentFd < 0) {
            return false;
        }

        // leaf自体はsymlink非追従のまま (leaf symlinkはsymlinkとして報告する契約)
        const bool ok = ::fstatat(parentFd, leafName.constData(), out,
                                  AT_SYMLINK_NOFOLLOW) == 0;
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        return ok;
    }

    bool isConfirmedAbsentAt(int dirFd, const QString &relativePath)
    {
        // 契約 (どのerrnoを不在と認めるか) は、filesystemhelpers.hを参照すること
        if (!validateRelativePathAt(relativePath, nullptr)) {
            return false;
        }

        const QStringList components =
                relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // 中間成分は fstatat(..., AT_SYMLINK_NOFOLLOW) でsymlinkを明示的に検出しながらopenat(..., O_DIRECTORY | O_NOFOLLOW)で辿る
        // O_DIRECTORY | O_NOFOLLOW は中間symlinkに対してENOTDIRを返し、「親が実ファイル」の場合と区別できないため、symlinkの検出をfstatat側で行う
        // fstatat()のAT_SYMLINK_NOFOLLOWはleafにしか効かないため、中間成分の解決をfstatat()に任せるとpin済みdirfdの外へ解決され得る
        int currentFd = ::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (currentFd < 0) {
            return false;
        }

        bool confirmedAbsent = false;
        for (int i = 0; i < components.size(); ++i) {
            const QByteArray encodedComponent = components.at(i).toUtf8();

            struct stat info;
            if (::fstatat(currentFd, encodedComponent.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                // ENOENTなら中間 / leafを問わず対象は確定的に存在しない
                // それ以外は不在を確認できていない
                confirmedAbsent = errno == ENOENT;
                break;
            }

            // symlink成分は、targetが存在してもしなくても追跡しない
            // 絶対targetならpin済みdirfdの外 (live filesystem) へ解決され得るため、「不在を確認できなかった」扱いとする (ELOOP相当)
            if (S_ISLNK(info.st_mode)) {
                errno = ELOOP;
                break;
            }

            if (i == components.size() - 1) {
                // leafが実在する (通常ファイル / ディレクトリ)
                break;
            }

            if (!S_ISDIR(info.st_mode)) {
                // 中間成分が非ディレクトリなら、その配下は確定的に存在し得ない
                confirmedAbsent = true;
                break;
            }

            const int nextFd = ::openat(currentFd, encodedComponent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (nextFd < 0) {
                // fstatatで確認したdirがopenatできない = 状態を確認できていない
                break;
            }
            ::close(currentFd);
            currentFd = nextFd;
        }

        ::close(currentFd);
        return confirmedAbsent;
    }

    int safeOpenRegularFileReadAt(int dirFd, const QString &relativePath)
    {
        if (dirFd < 0) {
            errno = EINVAL;
            return -1;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return -1;
        }

        // 検証済みの同一relativePathから親dirfdとleaf名を導出する (TOCTOUなし)
        const QStringList components =
                relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QByteArray leafName;
        const int parentFd = openParentDirectoryInRootAt(dirFd, components, &leafName);
        if (parentFd < 0) {
            return -1;
        }

        const int fd = ::openat(parentFd, leafName.constData(),
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        if (fd < 0) {
            return -1;
        }

        if (!validateRegularFileDescriptor(fd)) {
            const int validationErrno = errno;
            ::close(fd);
            errno = validationErrno;
            return -1;
        }

        return fd;
    }

    bool safeReadLinkNoFollowAt(int dirFd, const QString &relativePath, QByteArray *targetOut)
    {
        if (!targetOut || dirFd < 0) {
            errno = EINVAL;
            return false;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return false;
        }

        // 検証済みの同一relativePathから親dirfdとleaf名を導出する (TOCTOUなし)
        const QStringList components =
                relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QByteArray leafName;
        const int parentFd = openParentDirectoryInRootAt(dirFd, components, &leafName);
        if (parentFd < 0) {
            return false;
        }

        // 固定PATH_MAXで切り詰めないよう、必要に応じてバッファを拡張する
        QByteArray buffer(256, '\0');
        for (;;) {
            const ssize_t len = ::readlinkat(parentFd, leafName.constData(), buffer.data(),
                                             static_cast<size_t>(buffer.size()));
            if (len < 0) {
                const int savedErrno = errno;
                ::close(parentFd);
                errno = savedErrno;
                return false;
            }

            if (len < buffer.size()) {
                buffer.truncate(static_cast<qsizetype>(len));
                *targetOut = buffer;
                ::close(parentFd);
                return true;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    namespace detail {
        int openParentDirectoryInRootForTesting(int dirFd, const QString &relativePath,
                                                QByteArray *leafNameOut, bool forceNoOpenat2)
        {
            if (dirFd < 0) {
                errno = EINVAL;
                return -1;
            }

            if (!validateRelativePathAt(relativePath, nullptr)) {
                return -1;
            }

            const QStringList components =
                    relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            return openParentDirectoryInRootAt(dirFd, components, leafNameOut,
                                               forceNoOpenat2);
        }
    } // namespace detail

    /**
     * @brief 宛先絶対パスがrootPath配下であることを検証し、rootからの相対表現を取り出す
     *
     * 文字列レベルの入力解析を行うのみで、ファイルシステムにはアクセスしない
     * 本関数を通過しても安全は保証されない
     *
     * 実際の保証は、本関数の結果を用いて、root dirfdからcomponentwiseなO_NOFOLLOW走査を行う各変異ヘルパーが担う
     *
     * @param rootPath 基点ルートディレクトリ (絶対パス)
     * @param absolutePath 検証対象の宛先絶対パス
     * @param relativeOut rootPathからの相対表現の格納先
     * @return rootPath配下と確定した場合: true、拒否した場合: false (errno=EINVAL)
     */
    bool splitDestinationBeneathRoot(const QString &rootPath, const QString &absolutePath,
                                     QString *relativeOut)
    {
        if (!relativeOut) {
            errno = EINVAL;
            return false;
        }

        relativeOut->clear();

        if (!absolutePath.startsWith(QLatin1Char('/'))
                || !rootPath.startsWith(QLatin1Char('/'))) {
            errno = EINVAL;
            return false;
        }

        // 制御文字 (埋め込みNUL含む) はsyscall引数の切り詰めを招くため入力段階で拒否する
        if (containsControlChar(rootPath) || containsControlChar(absolutePath)) {
            errno = EINVAL;
            return false;
        }

        const QStringList rootComponents = rootPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList pathComponents =
                absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);

        // 宛先が空や"/"の場合は、leaf成分を持たないため宛先になり得ない
        if (pathComponents.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // "." / ".."成分による相対脱出を入力段階で拒否する
        for (const QString &component : pathComponents) {
            if (component == QLatin1String(".") || component == QLatin1String("..")) {
                errno = EINVAL;
                return false;
            }
        }

        // root自身は宛先にならない (leaf成分を最低1つ要求する)
        if (pathComponents.size() <= rootComponents.size()) {
            errno = EINVAL;
            return false;
        }

        // root成分列が真のプレフィックスであること (兄弟ディレクトリtrickを排除)
        for (int i = 0; i < rootComponents.size(); ++i) {
            if (pathComponents.at(i) != rootComponents.at(i)) {
                errno = EINVAL;
                return false;
            }
        }

        *relativeOut = pathComponents.mid(rootComponents.size()).join(QLatin1Char('/'));
        return true;
    }

    /**
     * @brief rootPathを基点に相対パスをO_NOFOLLOWで辿り、末端ディレクトリのfdを返す
     *
     * 呼び出し側は返されたfdを変異に使い、直ちにcloseすること
     * fdを認可(凍結)時点から実行時点へ跨いで保持してはならない
     *
     * @param rootPath 基点ルートディレクトリ (実ディレクトリであること)
     * @param relativePath rootPathからの相対パス ('/'開始や"." ".."成分は拒否)
     * @param createMissing 存在しない成分を作成するか
     * @param mode createMissing = true時の作成モード
     * @return 成功時: 末端ディレクトリのfd、失敗時: -1 (errno設定)
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode)
    {
        if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/'))
                || containsControlChar(relativePath)) {
            errno = EINVAL;
            return -1;
        }

        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            errno = EINVAL;
            return -1;
        }

        for (const QString &component : components) {
            if (component == QLatin1String(".") || component == QLatin1String("..")) {
                errno = EINVAL;
                return -1;
            }
        }

        const UniqueFd rootFd(openRootPathDirectory(rootPath));
        if (!rootFd.isValid()) {
            return -1;
        }

        return openDirectoryChainAt(rootFd.get(), components,
                                    static_cast<int>(components.size()), createMissing, mode);
    }

    /**
     * @brief rootPath配下に宛先ディレクトリを作成する (中間成分も必要に応じて作成)
     *
     * leafが既存の場合はfstatat(AT_SYMLINK_NOFOLLOW) で再確認し、ディレクトリ以外 (symlink含む) ならばEEXISTで拒否する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成するディレクトリのモード
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeCreateDirectoryBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                        mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, true, mode, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        if (::mkdirat(parentFd.get(), leafName.constData(), mode) < 0 && errno != EEXIST) {
            return false;
        }

        // 既存だった場合にsymlink等ではないことを変異時に再確認する
        struct stat st;
        if (::fstatat(parentFd.get(), leafName.constData(), &st, AT_SYMLINK_NOFOLLOW) < 0
                || !S_ISDIR(st.st_mode)) {
            errno = EEXIST;
            return false;
        }

        return true;
    }

    /**
     * @brief rootPath配下の通常ファイルを安全に新規/上書きオープンする
     *
     * leafは openat(..., O_NOFOLLOW) で開くためsymlinkならELOOPで拒否され、
     * 開いた後にfstat()でregular fileであることを再確認する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 対象ファイルの絶対パス (rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeOpenRegularFileWriteBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                            mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        const int fd = ::openat(parentFd.get(), leafName.constData(),
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd < 0) {
            return -1;
        }

        if (!validateRegularFileDescriptor(fd)) {
            const int validationErrno = errno;
            ::close(fd);
            errno = validationErrno;
            return -1;
        }

        return fd;
    }

    /**
     * @brief rootPath配下に通常ファイルを排他的に新規作成してオープンする
     *
     * leafはopenat(..., O_CREAT | O_EXCL | O_NOFOLLOW)で作成するため、既存エントリ (symlink含む) を掴むことはない
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeCreateRegularFileExclusiveBeneathRoot(const QString &rootPath,
                                                  const QString &destinationPath,
                                                  mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        const int fd = ::openat(parentFd.get(), leafName.constData(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd < 0) {
            return -1;
        }

        if (!validateRegularFileDescriptor(fd)) {
            const int validationErrno = errno;
            ::close(fd);
            errno = validationErrno;
            return -1;
        }

        return fd;
    }

    /**
     * @brief rootPath配下でsourcePathをdestinationPathへ移動する (rename-aside用)
     *
     * source / destinationはそれぞれ独立してrootから再解決されるため、
     * 片方の親だけが差し替えられた場合でも、変異は必ずroot配下に収まるか失敗する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param sourcePath 移動元の絶対パス (rootPath配下であること)
     * @param destinationPath 移動先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeRenamePathNoFollowBeneathRoot(const QString &rootPath, const QString &sourcePath,
                                           const QString &destinationPath)
    {
        QStringList sourceComponents;
        if (!splitDestinationComponents(rootPath, sourcePath, &sourceComponents)) {
            return false;
        }

        QStringList destinationComponents;
        if (!splitDestinationComponents(rootPath, destinationPath, &destinationComponents)) {
            return false;
        }

        QByteArray sourceLeaf;
        const UniqueFd sourceParentFd(
                openLeafParentBeneathRoot(rootPath, sourceComponents, false, 0, &sourceLeaf));
        if (!sourceParentFd.isValid()) {
            return false;
        }

        QByteArray destinationLeaf;
        const UniqueFd destinationParentFd(openLeafParentBeneathRoot(
                rootPath, destinationComponents, false, 0, &destinationLeaf));
        if (!destinationParentFd.isValid()) {
            return false;
        }

        return ::renameat(sourceParentFd.get(), sourceLeaf.constData(),
                          destinationParentFd.get(), destinationLeaf.constData()) == 0;
    }

    /**
     * @brief rootPath配下のパスをsymlink非追従で再帰削除する
     *
     * 親までの解決に失敗した場合は何も削除しない
     * leaf自体が存在しない場合のみENOENTを成功扱いとする (safeRemoveAllと同じ契約)
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除成功時: true、失敗時: false (errno設定)
     */
    bool safeRemoveAllBeneathRoot(const QString &rootPath, const QString &destinationPath)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return errno == ENOENT;
        }

        return removeEntryAt(parentFd.get(), leafName);
    }

    /**
     * @brief rootPath配下にsymlinkat()でsymlinkを作成する
     *
     * leaf位置が既存の場合 (symlink含む) はEEXISTで失敗する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param target 作成するsymlinkのtarget
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeCreateSymlinkNoFollowBeneathRoot(const QString &rootPath, const QByteArray &target,
                                              const QString &destinationPath)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        return ::symlinkat(target.constData(), parentFd.get(), leafName.constData()) == 0;
    }

    /**
     * @brief rootPath配下のsymlink自体のowner / timesをAT_SYMLINK_NOFOLLOWで更新する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param path 対象symlinkの絶対パス (rootPath配下であること)
     * @param owner 設定するowner UID
     * @param group 設定するgroup GID
     * @param times 設定するaccess / modification time
     * @param ownerUpdated owner更新成功結果の返却先
     * @param timesUpdated times更新成功結果の返却先
     * @return 両方成功した場合 true
     */
    bool safeSetSymlinkMetadataNoFollowBeneathRoot(const QString &rootPath, const QString &path,
                                                   uid_t owner, gid_t group,
                                                   const struct timespec times[2],
                                                   bool *ownerUpdated, bool *timesUpdated)
    {
        if (!ownerUpdated || !timesUpdated || !times) {
            errno = EINVAL;
            return false;
        }

        *ownerUpdated = false;
        *timesUpdated = false;

        QStringList components;
        if (!splitDestinationComponents(rootPath, path, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        bool allOk = true;
        if (::fchownat(parentFd.get(), leafName.constData(), owner, group,
                       AT_SYMLINK_NOFOLLOW) == 0) {
            *ownerUpdated = true;
        }
        else {
            allOk = false;
        }

        if (::utimensat(parentFd.get(), leafName.constData(), times, AT_SYMLINK_NOFOLLOW) == 0) {
            *timesUpdated = true;
        }
        else {
            allOk = false;
        }

        return allOk;
    }
} // namespace qsnapper::security
