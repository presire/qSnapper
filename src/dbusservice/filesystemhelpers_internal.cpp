/**
 * @file filesystemhelpers_internal.cpp
 * @brief filesystemhelpers系の共有内部helper実装
 *
 * パス検証・fd走査・in-root解決・削除などの内部helperを集める
 * 公開APIからは直接呼ばない
 */
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QStringList>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <vector>
#include "filesystemhelpers.h"
#include "filesystemhelpers_internal.h"

namespace qsnapper::security {
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
         * @brief statxの結果がmount境界 (別mountのroot、または親と異なるデバイス) を示すかを判定する
         *
         * STATX_ATTR_MOUNT_ROOTはattributes_maskで対応が示された場合のみ信用する
         * btrfsのネストしたsubvolumeはmountではないが、st_devが親と異なるため境界として扱う
         *
         * @param stx 判定対象のstatx結果
         * @param parentDev 親ディレクトリのst_dev
         * @return 境界である場合: true
         */
        bool isMountBoundary(const struct statx &stx, dev_t parentDev)
        {
            const bool mountRootKnown = (stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) != 0;
            if (mountRootKnown && (stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0) {
                return true;
            }
            return makedev(stx.stx_dev_major, stx.stx_dev_minor) != parentDev;
        }

        /**
         * @brief 親dirfd配下の1エントリを、symlink非追従かつmount境界を越えずに削除する
         *
         * 削除対象そのものと、降りていく各ディレクトリについてmount境界を確認する
         * 境界 (USB / NFS / FUSE / bind mount / ネストしたsubvolume等) を見つけた時点で降下を止め、errno=EXDEVで失敗を返す
         * 境界の内側は1件も削除しない
         * 境界を黙ってスキップして成功扱いにすると、呼び出し元は削除が完了したと誤認するため、必ず失敗として返す
         *
         * statxはAT_STATX_DONT_SYNCで呼び、FUSE / NFS等の境界先へ属性取得要求を送らない (応答しないFUSEでサービスが停止するのを防ぐ)
         *
         * @param parentFd 親ディレクトリfd
         * @param parentDev 親ディレクトリのst_dev
         * @param entryName 削除対象のleaf名
         * @return 削除成功時 (対象が存在しない場合を含む): true
         */
        bool removeEntryWithinDeviceAt(int parentFd, dev_t parentDev, const QByteArray &entryName)
        {
            constexpr int kStatxFlags = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC;
            struct statx stx;
            if (::statx(parentFd, entryName.constData(), kStatxFlags,
                        STATX_TYPE | STATX_INO, &stx) < 0) {
                return errno == ENOENT;
            }

            if (isMountBoundary(stx, parentDev)) {
                errno = EXDEV;
                return false;
            }

            // symlink / regular file / fifo / deviceは、unlinkat()側で削除する
            if (!S_ISDIR(stx.stx_mode)) {
                return ::unlinkat(parentFd, entryName.constData(), 0) == 0 || errno == ENOENT;
            }

            int childFd = ::openat(parentFd, entryName.constData(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (childFd < 0) {
                return false;
            }

            // statxからopenatまでの間に名前を差し替えられていないか、開いたディレクトリ自体が境界でないかを確認する
            struct statx opened;
            if (::statx(childFd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC,
                        STATX_TYPE | STATX_INO, &opened) < 0) {
                const int savedErrno = errno;
                ::close(childFd);
                errno = savedErrno;
                return false;
            }
            if (isMountBoundary(opened, parentDev) || opened.stx_ino != stx.stx_ino) {
                ::close(childFd);
                errno = EXDEV;
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

                if (!removeEntryWithinDeviceAt(childFd, parentDev, QByteArray(name))) {
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
         * @brief 親dirfd配下の1エントリを、symlink非追従かつmount境界を越えずに削除する
         *
         * 親ディレクトリと異なるデバイス上のエントリ (mount境界) を検出した場合はerrno=EXDEVで失敗する
         *
         * @param parentFd 親ディレクトリfd
         * @param entryName 削除対象のleaf名
         * @return 削除成功時: true
         */
        bool removeEntryAt(int parentFd, const QByteArray &entryName)
        {
            struct stat parentStat;
            if (::fstat(parentFd, &parentStat) < 0) {
                return false;
            }
            return removeEntryWithinDeviceAt(parentFd, parentStat.st_dev, entryName);
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
         * @brief 親dirfd上のleafを、regular fileである場合に限り読み取り用に開く
         *
         * FIFOやデバイスはopen()自体がブロックしたり副作用を起こしたりするため、開く前にfstatat()でregular fileであることを確認する
         * 確認とopenの間にFIFOへ差し替えられても待ち続けないよう、O_NONBLOCK | O_NOCTTYで開き (regular fileの読み込みには影響しない)、fstat()で再確認する
         *
         * @param parentFd 親ディレクトリのfd
         * @param leafName 親ディレクトリ内の名前
         * @return 成功時: file descriptor、失敗時: -1 (errno設定)
         */
        int openRegularFileReadInParent(int parentFd, const QByteArray &leafName)
        {
            struct stat leafStat;
            if (::fstatat(parentFd, leafName.constData(), &leafStat, AT_SYMLINK_NOFOLLOW) < 0) {
                return -1;
            }
            if (!S_ISREG(leafStat.st_mode)) {
                // symlinkはO_NOFOLLOWで開いた場合と同じELOOPを返し、従来のerrno契約を保つ
                errno = S_ISLNK(leafStat.st_mode) ? ELOOP : EINVAL;
                return -1;
            }

            const int fd = ::openat(parentFd, leafName.constData(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
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
                                        bool forceNoOpenat2)
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
} // namespace qsnapper::security
