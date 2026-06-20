#include <QByteArray>
#include <QStringList>
#include <dirent.h>
#include <fcntl.h>
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
         * @brief ルートディレクトリ "/" を開く
         *
         * @return 成功時: dirfd、失敗時: -1
         */
        int openRootDirectory()
        {
            return ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }

        /**
         * @brief パス成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く
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
            int dirFd = openRootDirectory();
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
} // namespace qsnapper::security
