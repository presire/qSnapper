/**
 * @file filesystemhelpers_replace.cpp
 * @brief 原子的差し替え・metadata適用・一覧指紋群
 *
 * 一時object作成からrename検証までの差し替え処理とmetadata適用、
 * snapshot一覧指紋を集める
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
    namespace {
        constexpr const char *kAclAccessXattr = "system.posix_acl_access";
        constexpr const char *kAclDefaultXattr = "system.posix_acl_default";
        constexpr const char *kCapabilityXattr = "security.capability";

        /**
         * @brief fdからxattrの値を読み出す
         *
         * 読み出しの間に値が伸びた場合 (ERANGE) は数回だけ再試行する
         *
         * @param fd 対象fd
         * @param name xattr名
         * @param valueOut 値の返却先
         * @return 1: 値あり、0: 属性なし (ENODATA)、-1: エラー (errno設定、ENOTSUPを含む)
         */
        int readXattrFd(int fd, const char *name, QByteArray *valueOut)
        {
            for (int attempt = 0; attempt < 4; ++attempt) {
                const ssize_t size = ::fgetxattr(fd, name, nullptr, 0);
                if (size < 0) {
                    return errno == ENODATA ? 0 : -1;
                }

                QByteArray value(static_cast<qsizetype>(size), '\0');
                const ssize_t length = ::fgetxattr(fd, name, value.data(), static_cast<size_t>(value.size()));
                if (length >= 0) {
                    value.truncate(static_cast<qsizetype>(length));
                    *valueOut = value;
                    return 1;
                }
                if (errno == ENODATA) {
                    return 0;
                }
                if (errno != ERANGE) {
                    return -1;
                }
            }

            errno = ERANGE;
            return -1;
        }

        /**
         * @brief POSIX ACLを復元元と一致させる
         *
         * 復元元にあれば設定し、無ければ復元先から削除する
         * 復元元のFSがACLに対応していない (ENOTSUP) 場合は「ACLなし」として扱う
         *
         * @param sourceFd 復元元fd
         * @param destinationFd 復元先fd
         * @param name ACLのxattr名
         * @return 一致させられた場合true (失敗時はerrno設定)
         */
        bool syncAclXattrFd(int sourceFd, int destinationFd, const char *name)
        {
            QByteArray value;
            const int state = readXattrFd(sourceFd, name, &value);
            if (state < 0 && errno != ENOTSUP) {
                return false;
            }

            if (state == 1) {
                return ::fsetxattr(destinationFd, name, value.constData(),
                                   static_cast<size_t>(value.size()), 0) == 0;
            }

            if (::fremovexattr(destinationFd, name) == 0 || errno == ENODATA || errno == ENOTSUP) {
                return true;
            }

            return false;
        }

        /**
         * @brief 必須・個別処理のxattr以外を、best-effortでコピーする
         * @param sourceFd 復元元fd
         * @param destinationFd 復元先fd
         * @return コピーできなかったxattrの件数 (一覧を取得できなかった場合は1)
         */
        int copyRemainingXattrsBestEffort(int sourceFd, int destinationFd)
        {
            QByteArray names;
            bool listed = false;
            for (int attempt = 0; attempt < 4 && !listed; ++attempt) {
                const ssize_t size = ::flistxattr(sourceFd, nullptr, 0);
                if (size < 0) {
                    return errno == ENOTSUP ? 0 : 1;
                }

                names = QByteArray(static_cast<qsizetype>(size), '\0');
                const ssize_t length = ::flistxattr(sourceFd, names.data(), static_cast<size_t>(names.size()));
                if (length >= 0) {
                    names.truncate(static_cast<qsizetype>(length));
                    listed = true;
                }
                else if (errno != ERANGE) {
                    return 1;
                }
            }
            if (!listed) {
                return 1;
            }

            int failures = 0;
            for (const QByteArray &name : names.split('\0')) {
                if (name.isEmpty() || name == kAclAccessXattr || name == kAclDefaultXattr
                        || name == kCapabilityXattr) {
                    continue;
                }

                QByteArray value;
                const int state = readXattrFd(sourceFd, name.constData(), &value);
                if (state == 0) {
                    continue;
                }
                if (state < 0
                        || ::fsetxattr(destinationFd, name.constData(), value.constData(),
                                       static_cast<size_t>(value.size()), 0) < 0) {
                    ++failures;
                }
            }

            return failures;
        }

        /**
         * @brief 宛先leaf名と同じディレクトリに置く一時leaf名を生成する
         *
         * 形式は "." + leaf + "." + tag + "." + pid + "." + ミリ秒 + "." + attempt
         * NAME_MAXを超える場合はleaf部分を文字単位で切り詰める (UTF-8の途中で切らない)
         *
         * @param leafName 宛先の最終成分名
         * @param tag 用途を示すASCIIのタグ
         * @param attempt 試行番号
         * @return 一時leaf名
         */
        QByteArray temporarySiblingLeafName(const QByteArray &leafName, const char *tag, int attempt)
        {
            struct timespec now;
            ::clock_gettime(CLOCK_REALTIME, &now);
            const qint64 milliseconds = static_cast<qint64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;

            const QByteArray suffix = QByteArray(".") + tag
                    + '.' + QByteArray::number(static_cast<qint64>(::getpid()))
                    + '.' + QByteArray::number(milliseconds)
                    + '.' + QByteArray::number(attempt);

            const qsizetype maxBaseBytes = static_cast<qsizetype>(NAME_MAX) - 1 - suffix.size();
            QString base = QString::fromUtf8(leafName);
            while (!base.isEmpty() && base.toUtf8().size() > maxBaseBytes) {
                base.chop(1);
            }

            return QByteArray(".") + base.toUtf8() + suffix;
        }

        /**
         * @brief 親dirfd上の名前が、期待するinodeを指しているか確認する
         * @param parentFd 親dirfd
         * @param name leaf名
         * @param expected 期待するinodeのstat
         * @return 同一inodeならtrue (不一致時はerrno=ESTALE)
         */
        bool isSameInodeAt(int parentFd, const QByteArray &name, const struct stat &expected)
        {
            struct stat current;
            if (::fstatat(parentFd, name.constData(), &current, AT_SYMLINK_NOFOLLOW) < 0) {
                return false;
            }
            if (current.st_dev != expected.st_dev || current.st_ino != expected.st_ino) {
                errno = ESTALE;
                return false;
            }
            return true;
        }

        /**
         * @brief 一時objectを、自分が作成したinodeを指している場合に限り削除する
         *
         * 差し替えられていた場合は他者のobjectを消さないよう、何もしない
         * errnoは呼び出し前の値を保持する
         *
         * @param parentFd 親dirfd
         * @param temporaryName 一時leaf名
         * @param created 作成直後に記録したstat
         */
        void discardTemporaryAt(int parentFd, const QByteArray &temporaryName, const struct stat &created)
        {
            const int savedErrno = errno;
            if (isSameInodeAt(parentFd, temporaryName, created)) {
                ::unlinkat(parentFd, temporaryName.constData(), 0);
            }
            errno = savedErrno;
        }

        /**
         * @brief rename直前の検証、renameat()、rename直後の検証を行う
         * @param parentFd 親dirfd
         * @param temporaryName 一時leaf名
         * @param leafName 宛先leaf名
         * @param created 作成直後に記録した一時objectのstat
         * @return 自分のinodeを宛先へ差し替えられた場合true (失敗時はerrno設定)
         */
        bool renameVerifiedAt(int parentFd, const QByteArray &temporaryName,
                              const QByteArray &leafName, const struct stat &created)
        {
            if (const auto &hook = detail::beforeReplaceRenameHook()) {
                hook(parentFd, temporaryName);
            }

            if (!isSameInodeAt(parentFd, temporaryName, created)) {
                // 差し替えられた一時名は自分のobjectではないため、削除もrenameもしない
                errno = ESTALE;
                return false;
            }

            if (::renameat(parentFd, temporaryName.constData(), parentFd, leafName.constData()) < 0) {
                discardTemporaryAt(parentFd, temporaryName, created);
                return false;
            }

            // rename直後に宛先が別objectへ差し替えられていないか確認する
            if (!isSameInodeAt(parentFd, leafName, created)) {
                errno = ESTALE;
                return false;
            }

            return true;
        }

        /**
         * @brief 親dirfdと宛先leaf名の引数を検証する
         * @param parentFd 親dirfd
         * @param leafName 宛先leaf名
         * @return 妥当な場合true (不正時はerrno=EINVAL)
         */
        bool isValidReplaceTarget(int parentFd, const QByteArray &leafName)
        {
            if (parentFd < 0 || leafName.isEmpty() || leafName == "." || leafName == ".."
                    || leafName.contains('/') || leafName.contains('\0')) {
                errno = EINVAL;
                return false;
            }
            return true;
        }
    }

    int safeOpenDirectoryReadAt(int dirFd, const QString &relativePath)
    {
        if (dirFd < 0) {
            errno = EINVAL;
            return -1;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return -1;
        }

        // 検証済みの同一relativePathから親dirfdとleaf名を導出する (TOCTOUなし)
        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QByteArray leafName;
        const UniqueFd parentFd(openParentDirectoryInRootAt(dirFd, components, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        return ::openat(parentFd.get(), leafName.constData(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }

    int openDestinationParentBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                         QByteArray *leafNameOut)
    {
        if (!leafNameOut) {
            errno = EINVAL;
            return -1;
        }

        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        return openLeafParentBeneathRoot(rootPath, components, false, 0, leafNameOut);
    }

    RestoredMetadataResult applyRestoredMetadata(int sourceFd, int destinationFd, bool isDirectory)
    {
        RestoredMetadataResult result;
        const auto recordMandatoryFailure = [&result]() {
            if (!result.mandatoryFailed) {
                result.mandatoryFailed = true;
                result.mandatoryErrno = errno;
            }
        };

        struct stat sourceStat;
        if (::fstat(sourceFd, &sourceStat) < 0) {
            recordMandatoryFailure();
            return result;
        }

        // 所有者の変更はS_ISUID / S_ISGIDとsecurity.capabilityを落とすため、modeとcapabilityより先に行う
        if (::fchown(destinationFd, sourceStat.st_uid, sourceStat.st_gid) < 0) {
            recordMandatoryFailure();
        }
        if (::fchmod(destinationFd, sourceStat.st_mode & 07777) < 0) {
            recordMandatoryFailure();
        }

        if (!syncAclXattrFd(sourceFd, destinationFd, kAclAccessXattr)) {
            recordMandatoryFailure();
        }
        if (isDirectory && !syncAclXattrFd(sourceFd, destinationFd, kAclDefaultXattr)) {
            recordMandatoryFailure();
        }

        // ACLの設定はgroupのpermission bitを書き換えるため、最終的なmodeを復元元と一致させる
        struct stat destinationStat;
        if (::fstat(destinationFd, &destinationStat) == 0
                && (destinationStat.st_mode & 07777) != (sourceStat.st_mode & 07777)
                && ::fchmod(destinationFd, sourceStat.st_mode & 07777) < 0) {
            recordMandatoryFailure();
        }

        result.optionalFailures = copyRemainingXattrsBestEffort(sourceFd, destinationFd);

        // file capabilityは所有者とmodeの変更で失われるため、最後に設定する
        // 新規inodeには存在しないため、復元元に無い場合の削除は不要
        if (!isDirectory) {
            QByteArray capability;
            const int state = readXattrFd(sourceFd, kCapabilityXattr, &capability);
            if (state < 0 && errno != ENOTSUP) {
                recordMandatoryFailure();
            }
            else if (state == 1
                     && ::fsetxattr(destinationFd, kCapabilityXattr, capability.constData(),
                                    static_cast<size_t>(capability.size()), 0) < 0) {
                recordMandatoryFailure();
            }
        }

        return result;
    }

    bool replaceRegularFileAt(int destinationParentFd, const QByteArray &leafName, int sourceFd,
                              bool tryReflink, bool mustPreserveMetadata,
                              RestoredMetadataResult *metadataOut)
    {
        if (!isValidReplaceTarget(destinationParentFd, leafName) || sourceFd < 0) {
            errno = EINVAL;
            return false;
        }

        struct stat sourceStat;
        if (::fstat(sourceFd, &sourceStat) < 0) {
            return false;
        }
        if (!S_ISREG(sourceStat.st_mode)) {
            errno = EINVAL;
            return false;
        }

        // 権限を絞った状態 (0600) で作成し、所有者とmodeは後からfd経由で設定する
        QByteArray temporaryName;
        int rawFd = -1;
        for (int attempt = 0; attempt < 16 && rawFd < 0; ++attempt) {
            temporaryName = temporarySiblingLeafName(leafName, "qsnapper-copy", attempt);
            rawFd = ::openat(destinationParentFd, temporaryName.constData(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (rawFd < 0 && errno != EEXIST) {
                return false;
            }
        }
        if (rawFd < 0) {
            return false;
        }

        UniqueFd destinationFd(rawFd);
        struct stat created;
        if (::fstat(destinationFd.get(), &created) < 0) {
            return false;
        }

        const auto abortReplace = [&]() {
            const int savedErrno = errno;
            destinationFd.reset();
            discardTemporaryAt(destinationParentFd, temporaryName, created);
            errno = savedErrno;
            return false;
        };

        bool copied = tryReflink && ::ioctl(destinationFd.get(), FICLONE, sourceFd) == 0;
        if (!copied) {
            off_t offset = 0;
            off_t remaining = sourceStat.st_size;
            while (remaining > 0) {
                const ssize_t written = ::sendfile(destinationFd.get(), sourceFd, &offset,
                                                   static_cast<size_t>(remaining));
                if (written < 0) {
                    return abortReplace();
                }
                if (written == 0) {
                    // 期待したbyte数に達する前にEOFになった
                    errno = EIO;
                    return abortReplace();
                }
                remaining -= written;
            }
        }

        const RestoredMetadataResult metadata = applyRestoredMetadata(sourceFd, destinationFd.get(), false);
        if (metadataOut) {
            *metadataOut = metadata;
        }
        if (metadata.mandatoryFailed && mustPreserveMetadata) {
            errno = metadata.mandatoryErrno;
            return abortReplace();
        }

        const struct timespec times[2] = { sourceStat.st_atim, sourceStat.st_mtim };
        if (::futimens(destinationFd.get(), times) < 0 && mustPreserveMetadata) {
            return abortReplace();
        }

        destinationFd.reset();
        return renameVerifiedAt(destinationParentFd, temporaryName, leafName, created);
    }

    bool replaceSymlinkAt(int destinationParentFd, const QByteArray &leafName,
                          const QByteArray &linkTarget, const struct stat &sourceStat,
                          bool *metadataApplied)
    {
        if (metadataApplied) {
            *metadataApplied = false;
        }
        if (!isValidReplaceTarget(destinationParentFd, leafName) || linkTarget.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        QByteArray temporaryName;
        bool createdLink = false;
        for (int attempt = 0; attempt < 16 && !createdLink; ++attempt) {
            temporaryName = temporarySiblingLeafName(leafName, "qsnapper-link", attempt);
            if (::symlinkat(linkTarget.constData(), destinationParentFd, temporaryName.constData()) == 0) {
                createdLink = true;
            }
            else if (errno != EEXIST) {
                return false;
            }
        }
        if (!createdLink) {
            return false;
        }

        // 作成した一時symlink自体をfdで固定し、以降の操作で名前を再解決しない
        const UniqueFd linkFd(::openat(destinationParentFd, temporaryName.constData(),
                                       O_PATH | O_NOFOLLOW | O_CLOEXEC));
        if (!linkFd.isValid()) {
            return false;
        }

        struct stat created;
        if (::fstat(linkFd.get(), &created) < 0) {
            return false;
        }

        // 作成直後に差し替えられていないか (symlink、所有者がeuid、targetが一致) を確認する
        QByteArray currentTarget(linkTarget.size() + 1, '\0');
        const ssize_t targetLength = ::readlinkat(linkFd.get(), "", currentTarget.data(),
                                                  static_cast<size_t>(currentTarget.size()));
        if (!S_ISLNK(created.st_mode) || created.st_uid != ::geteuid()
                || targetLength != static_cast<ssize_t>(linkTarget.size())
                || currentTarget.left(targetLength) != linkTarget) {
            errno = ESTALE;
            return false;
        }

        const bool ownerUpdated = ::fchownat(linkFd.get(), "", sourceStat.st_uid, sourceStat.st_gid,
                                       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == 0;
        const struct timespec times[2] = { sourceStat.st_atim, sourceStat.st_mtim };
        bool timesUpdated = ::utimensat(linkFd.get(), "", times, AT_EMPTY_PATH) == 0;
        if (!timesUpdated && errno == EINVAL) {
            // utimensat()のAT_EMPTY_PATHに対応していないカーネル向け:
            // 同じ親dirfd上の一時名へ適用し、前後で同じinodeであることを確認する
            timesUpdated = isSameInodeAt(destinationParentFd, temporaryName, created)
                    && ::utimensat(destinationParentFd, temporaryName.constData(), times, AT_SYMLINK_NOFOLLOW) == 0
                    && isSameInodeAt(destinationParentFd, temporaryName, created);
        }
        if (metadataApplied) {
            *metadataApplied = ownerUpdated && timesUpdated;
        }

        return renameVerifiedAt(destinationParentFd, temporaryName, leafName, created);
    }

    QByteArray snapshotListFingerprint(const QString &snapshotsDirPath)
    {
        const int dirFd = ::open(snapshotsDirPath.toUtf8().constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            return {};
        }

        DIR *dir = ::fdopendir(dirFd);
        if (!dir) {
            ::close(dirFd);
            return {};
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        const auto addValue = [&hash](qint64 value) {
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(&value), sizeof(value)));
        };
        const auto addStat = [&addValue](const struct stat &st) {
            addValue(static_cast<qint64>(st.st_dev));
            addValue(static_cast<qint64>(st.st_ino));
            addValue(static_cast<qint64>(st.st_size));
            addValue(static_cast<qint64>(st.st_mtim.tv_sec));
            addValue(static_cast<qint64>(st.st_mtim.tv_nsec));
            addValue(static_cast<qint64>(st.st_ctim.tv_sec));
            addValue(static_cast<qint64>(st.st_ctim.tv_nsec));
        };

        struct stat dirStat;
        if (::fstat(dirFd, &dirStat) < 0) {
            ::closedir(dir);
            return {};
        }
        addStat(dirStat);

        // libsnapperは数字だけの名前のエントリをスナップショットとして扱う
        // 先頭に0が付いた名前 ("01"など) も含めるため、数値に変換せず名前のまま比較する
        std::vector<QByteArray> names;
        errno = 0;
        while (const struct dirent *entry = ::readdir(dir)) {
            const QByteArray name(entry->d_name);
            const bool allDigits = !name.isEmpty()
                    && std::all_of(name.cbegin(), name.cend(),
                                   [](char c) { return c >= '0' && c <= '9'; });
            if (allDigits) {
                names.push_back(name);
            }
            errno = 0;
        }
        if (errno != 0) {
            ::closedir(dir);
            return {};
        }
        std::sort(names.begin(), names.end());

        for (const QByteArray &name : names) {
            // 名前の境界が曖昧にならないように長さも指紋へ含める
            addValue(static_cast<qint64>(name.size()));
            hash.addData(name);

            struct stat entryStat;
            if (::fstatat(dirFd, name.constData(), &entryStat, AT_SYMLINK_NOFOLLOW) < 0) {
                ::closedir(dir);
                return {};
            }
            addStat(entryStat);

            // 作成途中でinfo.xmlがまだ無いエントリは、無いことを指紋に含める (後から作られると指紋が変わる)
            struct stat infoStat;
            const QByteArray infoPath = name + QByteArrayLiteral("/info.xml");
            if (::fstatat(dirFd, infoPath.constData(), &infoStat, AT_SYMLINK_NOFOLLOW) == 0) {
                addStat(infoStat);
            }
            else if (errno == ENOENT || errno == ENOTDIR) {
                addValue(-1);
            }
            else {
                ::closedir(dir);
                return {};
            }
        }

        ::closedir(dir);
        return hash.result();
    }
} // namespace qsnapper::security
