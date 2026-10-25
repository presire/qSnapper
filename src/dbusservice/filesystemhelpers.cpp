/**
 * @file filesystemhelpers.cpp
 * @brief 絶対パス起点とdirfd相対の安全オープン・stat・存在観測
 *
 * symlinkを辿らない読み取り専用プリミティブと単体テスト用のdetail入口を提供する
 * observePathNoFollowAtは、パスがdirfdの木に含まれるかを存在・不在・不明の3値で返す
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

        const int fd = openRegularFileReadInParent(parentFd, leafName);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
        return fd;
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

    PathPresenceAt observePathNoFollowAt(int dirFd, const QString &relativePath, struct stat *out)
    {
        // 判定の規則は、filesystemhelpers.hを参照すること
        if (!out || dirFd < 0) {
            errno = EINVAL;
            return PathPresenceAt::Unknown;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return PathPresenceAt::Unknown;
        }

        const QStringList components =
                relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            errno = EINVAL;
            return PathPresenceAt::Unknown;
        }

        // 根のst_devを取得できないとmount境界を判定できないため、存在を確定しない
        struct stat rootInfo;
        if (::fstat(dirFd, &rootInfo) != 0) {
            return PathPresenceAt::Unknown;
        }

        // 走査用のfdを複製できない場合は何も観測できない
        int currentFd = ::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (currentFd < 0) {
            return PathPresenceAt::Unknown;
        }

        PathPresenceAt result = PathPresenceAt::Unknown;
        for (int i = 0; i < components.size(); ++i) {
            const QByteArray encodedComponent = components.at(i).toUtf8();

            struct stat info;
            if (::fstatat(currentFd, encodedComponent.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                // 不在と断定できるのはENOENTだけで、EACCES等は存在を確認できなかった扱いにする
                result = errno == ENOENT ? PathPresenceAt::Absent : PathPresenceAt::Unknown;
                break;
            }

            if (i == components.size() - 1) {
                *out = info;
                result = PathPresenceAt::Present;
                break;
            }

            // symlinkを含む非ディレクトリの下は、libsnapperの比較が辿らないため木に含まれない
            if (!S_ISDIR(info.st_mode)) {
                result = PathPresenceAt::Absent;
                break;
            }

            // 別のファイルシステムの下は、存在しないとは言えないため判定しない (理由はfilesystemhelpers.hを参照)
            if (info.st_dev != rootInfo.st_dev) {
                errno = EXDEV;
                break;
            }

            const int nextFd = ::openat(currentFd, encodedComponent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (nextFd < 0) {
                // fstatat()で見えた直後に開けない場合は差し替えか権限変更があり、観測結果を信用できない
                break;
            }

            // fstatat()の後に同じ名前が別のディレクトリへ差し替えられていれば、観測結果を信用できない
            // 同じデバイスのbind mountはst_devでは区別できないため、mountのrootかどうかもstatxで確認する
            struct statx openedInfo;
            if (::statx(nextFd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO, &openedInfo) != 0
                    || makedev(openedInfo.stx_dev_major, openedInfo.stx_dev_minor) != info.st_dev
                    || openedInfo.stx_ino != info.st_ino) {
                ::close(nextFd);
                errno = ESTALE;
                break;
            }

            if (isMountBoundary(openedInfo, rootInfo.st_dev)) {
                ::close(nextFd);
                errno = EXDEV;
                break;
            }
            ::close(currentFd);
            currentFd = nextFd;
        }

        // close()がerrnoを上書きするため、判定の理由を呼び出し元へそのまま返す
        const int savedErrno = errno;
        ::close(currentFd);
        errno = savedErrno;
        return result;
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

        const int fd = openRegularFileReadInParent(parentFd, leafName);
        const int savedErrno = errno;
        ::close(parentFd);
        errno = savedErrno;
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

        /**
         * @brief rename直前フックの保存先
         * @return フックへの参照
         */
        std::function<void(int, const QByteArray &)> &beforeReplaceRenameHook()
        {
            static std::function<void(int, const QByteArray &)> hook;
            return hook;
        }

        void setBeforeReplaceRenameHookForTesting(
                std::function<void(int parentFd, const QByteArray &temporaryName)> hook)
        {
            beforeReplaceRenameHook() = std::move(hook);
        }
    } // namespace detail
} // namespace qsnapper::security
