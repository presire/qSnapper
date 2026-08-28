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
         * コピー禁止。エラー路径を含めて全てのfd解放を保証する
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
         * 空パス・絶対パス・"." / ".." 成分・制御文字を拒否する。
         * openat系は絶対パスを渡すとdirfdを無視するため、pin済みdirfdから
         * 意図しない位置へ解決されるのを入力段階で防ぐ
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
         * 単純なopen(rootPath, O_NOFOLLOW)は末尾成分にしかO_NOFOLLOWが効かず、
         * rootPathの中間成分がsymlink化されていた場合に追従してしまう。
         * そのため既存のsafeOpenDirectory() (「/」起点の成分単位no-follow walk) へ委譲し、
         * root祖先の差し替えも拒否する
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
         * baseFdの所有は取らない (openat(baseFd, ".") で同一ディレクトリの所有fdを得る)。
         * 成功時は呼び出し側がcloseすべきleaf fd、失敗時は-1を返す
         *
         * @param baseFd 走査基点のディレクトリfd
         * @param components 相対パス成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分を mkdirat() で作成するか
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
         * root自身はO_NOFOLLOWで開くため、rootがsymlinkへ差し替えられている場合も拒否する。
         * 中間成分は openat(..., O_DIRECTORY | O_NOFOLLOW) の連鎖で解決するため、
         * どの階層のsymlink差し替えでもELOOPで失敗する
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

            // symlink / regular file / fifo / deviceは unlinkat() 側で削除する
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

        // 親ディレクトリ fd を固定した openat() により leaf を作成/上書きする。
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
     * @brief 親dirfdを固定した renameat() でパスを移動する
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
     * @brief symlink targetを readlinkat() で取得する
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
     * @brief symlinkat() を用いてsymlinkを作成する
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
     * @brief read-only用途で lstat() を行う
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

        // 契約および使用上の制約は、filesystemhelpers.h を参照すること
        const QByteArray encodedPath = path.toUtf8();
        return ::lstat(encodedPath.constData(), out) == 0;
    }

    bool safeLstatAt(int dirFd, const QString &relativePath, struct stat *out)
    {
        if (!out || dirFd < 0) {
            errno = EINVAL;
            return false;
        }

        QByteArray encodedPath;
        if (!validateRelativePathAt(relativePath, &encodedPath)) {
            return false;
        }

        return ::fstatat(dirFd, encodedPath.constData(), out,
                         AT_SYMLINK_NOFOLLOW) == 0;
    }

    int safeOpenRegularFileReadAt(int dirFd, const QString &relativePath)
    {
        if (dirFd < 0) {
            errno = EINVAL;
            return -1;
        }

        QByteArray encodedPath;
        if (!validateRelativePathAt(relativePath, &encodedPath)) {
            return -1;
        }

        const int fd = ::openat(dirFd, encodedPath.constData(),
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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

    bool safeReadLinkNoFollowAt(int dirFd, const QString &relativePath,
                                QByteArray *targetOut)
    {
        if (!targetOut || dirFd < 0) {
            errno = EINVAL;
            return false;
        }

        QByteArray encodedPath;
        if (!validateRelativePathAt(relativePath, &encodedPath)) {
            return false;
        }

        // 固定PATH_MAXで切り詰めないよう、必要に応じてバッファを拡張する
        QByteArray buffer(256, '\0');
        for (;;) {
            const ssize_t len = ::readlinkat(dirFd, encodedPath.constData(),
                                             buffer.data(),
                                             static_cast<size_t>(buffer.size()));
            if (len < 0) {
                return false;
            }

            if (len < buffer.size()) {
                buffer.truncate(static_cast<qsizetype>(len));
                *targetOut = buffer;
                return true;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    /**
     * @brief 宛先絶対パスがrootPath配下であることを検証し、rootからの相対表現を取り出す
     *
     * 文字列レベルの入力解析を行うのみで、ファイルシステムにはアクセスしない。
     * 本関数を通過しても安全は保証されない。実際の保証は、本関数の結果を用いて
     * root dirfdからcomponentwiseなO_NOFOLLOW走査を行う各変異ヘルパーが担う
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

        // 宛先が空や "/" の場合はleaf成分を持たないため宛先になり得ない
        if (pathComponents.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // "." / ".." 成分による相対脱出を入力段階で拒否する
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
     * 呼び出し側は返されたfdを変異に使い、直ちにcloseすること。
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
     * leafが既存の場合は fstatat(AT_SYMLINK_NOFOLLOW) で再確認し、
     * ディレクトリ以外 (symlink含む) ならばEEXISTで拒否する
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
     * 開いた後に fstat() でregular fileであることを再確認する
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
     * leafは openat(..., O_CREAT | O_EXCL | O_NOFOLLOW) で作成するため、
     * 既存エントリ (symlink含む) を掴むことはない
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
     * @brief rootPath配下で sourcePath を destinationPath へ移動する (rename-aside用)
     *
     * source / destination はそれぞれ独立してrootから再解決されるため、
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
     * 親までの解決に失敗した場合は何も削除しない。
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
     * @brief rootPath配下に symlinkat() でsymlinkを作成する
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
