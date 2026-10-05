/**
 * @file snapshotoperations_restore_apply.cpp
 * @brief 凍結済み復元計画のlive filesystemへの適用
 *
 * 退避先兄弟パスの解決、entry単位の適用
 * ディレクトリ作成と通常ファイルやsymlinkの差し替えを扱う
 * 適用はpin済みdirfd相対で行い、実行中の置き換えに左右されない
 */

#include <QDebug>
#include <QDateTime>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utime.h>
#include <errno.h>
#include <sys/xattr.h>

/**
 * @brief live pathをroot配下で再解決して一時的な兄弟pathへ退避する
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param path 退避対象の絶対path
 * @param movedPath 実際の退避先
 *                  対象不存在時は空文字列
 * @return 退避または対象不存在時true
 */
bool SnapshotOperations::movePathAsideBeneathRoot(const QString &rootPath,
                                                  const QString &path,
                                                  QString *movedPath)
{
    if (movedPath) {
        movedPath->clear();
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = qsnapper::internal::siblingTemporaryPath(
            path, QStringLiteral("qsnapper-old"), attempt);
        if (qsnapper::security::safeRenamePathNoFollowBeneathRoot(rootPath, path, candidate)) {
            if (movedPath) {
                *movedPath = candidate;
            }
            return true;
        }

        if (errno == ENOENT) {
            return true;
        }

        // 退避先の名前が既に使われている場合 (RENAME_NOREPLACEによりEEXIST) は次の候補を試す
        // RENAME_NOREPLACE未対応 (EINVAL / ENOSYS) を含むその他のエラーでは、live側に触れずに失敗する
        if (errno == EEXIST || errno == ENOTEMPTY) {
            continue;
        }
        return false;
    }

    errno = EEXIST;
    return false;
}

/**
 * @brief executor callbackから単一の凍結済みentryをlive filesystemへ適用する
 * @param manifestId 実行contextを選択するマニフェストID
 * @param entry 適用対象entry
 * @return entry全体の適用成功時true
 */
bool SnapshotOperations::applyRestoreEntry(
    const QString &manifestId,
    const qsnapper::restore::RestoreEntry &entry)
{
    const auto execution = m_restoreExecutions.constFind(manifestId);
    if (execution == m_restoreExecutions.cend()) {
        qWarning() << "Staged restore: Missing execution context";
        return false;
    }
    const RestoreExecution context = execution.value();
    if (context.snapshotDirFd < 0) {
        qWarning() << "Staged restore: Snapshot source fd is not pinned";
        return false;
    }

    const bool validChangeType = entry.changeType == QStringLiteral("created")
            || entry.changeType == QStringLiteral("deleted")
            || entry.changeType == QStringLiteral("modified")
            || entry.changeType == QStringLiteral("typechanged");
    // entry.pathはconfig相対名である
    // live側の宛先はcommit時に固定したSUBVOLUME配下に組み立て、以降の全てのlive操作はrootPathを基準に解決する
    // relativePathはrootPathからの相対であり、同時にpin済みスナップショット dirfdからの相対でもある
    QString rootPath;
    QString destinationPath;
    QString relativePath;
    if (!validChangeType
            || qsnapper::restore::isSnapshotMetadataRestoreName(entry.path)
            || !qsnapper::restore::buildRestoreDestination(
                context.subvolume, entry.path, &rootPath, &destinationPath, &relativePath)) {
        qWarning() << "Staged restore: Frozen entry failed execution-time validation";
        return false;
    }

    const QString snapshotFilePath = context.snapshotDir + QLatin1Char('/') + relativePath;
    if (!qsnapper::security::isPathWithinSnapshotRoot(
            snapshotFilePath, context.snapshotDir)) {
        qWarning() << "Staged restore: Source escaped snapshot root";
        return false;
    }

    if (entry.changeType == QStringLiteral("created")) {
        // "created"は「復元元snapshotに存在しない」という意味であり、だからこそlive側からの削除が正しい動作になる
        // 逆に復元元に存在するパスは、削除ではなく復元が正しい
        // サーバはクライアントが申告したentryを再計算しないため、シリアライズ経路への注入等で偽装されたentryがここまで到達し得る
        // 破壊の直前に前提そのものを確認する
        //
        // 検証は認可時にpinしたsnapshotDirFd相対で行い、本関数の冒頭でbuildRestoreDestinationがentry.pathから導出したrelativePathをそのまま使用する
        // 復元元snapshotはread-onlyかつfdでpin済みであり、検証対象と削除対象は同一のentry.pathに由来するため、検証後に別の値を取り直す余地はない
        if (!qsnapper::security::isConfirmedAbsentAt(context.snapshotDirFd, relativePath)) {
            qWarning() << "Staged restore: Refused to delete a path that is not confirmed"
                       << "absent from the restore source";
            return false;
        }

        const bool removed = qsnapper::security::safeRemoveAllBeneathRoot(rootPath, destinationPath);
        if (!removed) {
            qWarning() << "Staged restore: Failed to remove live path:"
                       << strerror(errno);
        }
        return removed;
    }

    // ソースの種別判定はpin済みsnapshot dirfd相対で行う
    // 実行中にsnapshotDirのpath上で何が起きても、認可時にpinしたinodeを観測する
    // (AT_SYMLINK_NOFOLLOWによりleafのsymlinkも展開しない)
    struct stat snapshotFileInfo;
    const bool hasSnapshotFileInfo = qsnapper::security::safeLstatAt(
        context.snapshotDirFd, relativePath, &snapshotFileInfo);

    // live側を破壊する前に復元元の可用性と種別を確定させる
    // 凍結・認可の時点でsnapshotDirを検証していても、認可から本entryの適用までにはchunk境界でevent loopが回るため、
    // その間に復元元snapshotが削除 / unmountされ得る
    // 先に退避・削除してから "Source is not restorable" で失敗すると、復元元が存在しないままlive側のデータだけが失われる
    const bool sourceIsLink = hasSnapshotFileInfo && S_ISLNK(snapshotFileInfo.st_mode);
    const bool sourceIsDirectory = hasSnapshotFileInfo && S_ISDIR(snapshotFileInfo.st_mode);
    const bool sourceIsRegular = hasSnapshotFileInfo && S_ISREG(snapshotFileInfo.st_mode);
    if (!sourceIsLink && !sourceIsDirectory && !sourceIsRegular) {
        qWarning() << "Staged restore: Source is not restorable";
        return false;
    }

    // 復元対象ではない欠けた親ディレクトリは、snapshot側の同じディレクトリに合わせて作成する
    // snapshot側に無い場合は0700・root所有のままにし、既存の親ディレクトリは変更しない
    const int slashIndex = destinationPath.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = slashIndex <= 0 ? QStringLiteral("/")
                                               : destinationPath.left(slashIndex);
    if (parentPath != rootPath
            && !qsnapper::security::safeCreateParentDirectoriesFromSourceBeneathRoot(
                rootPath, parentPath, context.snapshotDirFd, ::geteuid() == 0)) {
        qWarning() << "Staged restore: Failed to create live parent directory:"
                   << strerror(errno);
        return false;
    }

    QString detachedPath;
    if (context.removeOnTypechanged && entry.changeType == QStringLiteral("typechanged")) {
        if (!movePathAsideBeneathRoot(rootPath, destinationPath, &detachedPath)) {
            qWarning() << "Staged restore: Failed to move live path aside:"
                       << strerror(errno);
            return false;
        }
    }

    bool applied = false;
    if (sourceIsLink) {
        applied = copySymlinkBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath);
    }
    else if (sourceIsDirectory) {
        applied = applyDirectoryBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath);
    }
    else {
        applied = copyRegularFileBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath, context.useReflink);
    }

    // 退避物の破棄は復元成功後にのみ行う
    // 失敗時は元の位置へ戻し、復元できなかったlive側のデータを消さない
    // 戻すことすらできない場合も退避物は削除せず、復旧できるようpathを記録する
    if (!detachedPath.isEmpty()) {
        if (applied) {
            if (!qsnapper::security::safeRemoveAllBeneathRoot(rootPath, detachedPath)) {
                qWarning() << "Staged restore: Failed to remove detached live path:"
                           << strerror(errno);
            }
        }
        else if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(rootPath, detachedPath, destinationPath)) {
            qCritical() << "Staged restore: Failed to reattach live path after a failed"
                        << "restore. Previous content is preserved at:" << detachedPath;
        }
    }

    return applied;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、ディレクトリを作成してmetadataを適用する
 *
 * 作成 (mkdirat) と、metadataを適用するためのopenを同じ親dirfd上で行い、名前を再解決しない
 * 既存のディレクトリはそのまま使い、所有者・mode・ACL (access / default)・xattrをsnapshot側に合わせる
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @return ディレクトリと必須metadataを適用できた場合true
 */
bool SnapshotOperations::applyDirectoryBeneathRoot(int sourceDirFd,
                                                   const QString &sourceRelativePath,
                                                   const QString &rootPath,
                                                   const QString &dst)
{
    const int srcFd = qsnapper::security::safeOpenDirectoryReadAt(sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to open source directory:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        ::close(srcFd);
        return false;
    }

    // 作成時は権限を絞り (0700)、所有者とmodeは後からfd経由で設定する
    int directoryFd = -1;
    if (::mkdirat(parentFd, leafName.constData(), 0700) == 0 || errno == EEXIST) {
        directoryFd = ::openat(parentFd, leafName.constData(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    const int openErrno = errno;
    ::close(parentFd);
    if (directoryFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to create or open live directory:"
                   << strerror(openErrno);
        ::close(srcFd);
        return false;
    }

    const qsnapper::security::RestoredMetadataResult metadata =
        qsnapper::security::applyRestoredMetadata(srcFd, directoryFd, /*isDirectory=*/true);
    ::close(directoryFd);
    ::close(srcFd);

    if (metadata.optionalFailures > 0) {
        qWarning() << "applyDirectoryBeneathRoot:" << metadata.optionalFailures
                   << "extended attribute(s) could not be copied (non-fatal)";
    }
    if (metadata.mandatoryFailed) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to preserve owner, mode or ACL:"
                   << strerror(metadata.mandatoryErrno);
        return ::geteuid() != 0;
    }

    return true;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、通常ファイルをコピーして差し替える
 *
 * live側を直接O_TRUNCで開くと、実行中のバイナリ (復元を実行しているqSnapper自身や稼働中のサービス) がETXTBSYで拒否され、既存inodeのhardlinkや緩いmodeも引き継いでしまう
 * そのため、同じ親dirfd上の一時ファイルへ書き出し、data → 所有者 → mode → ACL → xattr → capability → 時刻をfd経由で適用してから、renameat()で差し替える
 * 一時ファイルの作成からrenameまで名前を再解決しないため、書き込み可能な親ディレクトリを制御する攻撃者が途中で一時名や親を差し替えても、
 * 別objectをrenameしたり、攻撃者のobjectへmetadataを適用したりしない (差し替えはrenameの前後で検出して失敗にする)
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @param tryReflink FICLONEを先行試行するか
 * @return dataと必須metadataを適用して差し替えできた場合true
 */
bool SnapshotOperations::copyRegularFileBeneathRoot(int sourceDirFd,
                                                    const QString &sourceRelativePath,
                                                    const QString &rootPath,
                                                    const QString &dst,
                                                    bool tryReflink)
{
    const int srcFd = qsnapper::security::safeOpenRegularFileReadAt(
        sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to open source:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        ::close(srcFd);
        return false;
    }

    // rootで動作する本番環境では、所有者・mode・ACL・capability・時刻を保てない復元は失敗扱いにする
    const bool mustPreserveMetadata = (::geteuid() == 0);
    qsnapper::security::RestoredMetadataResult metadata;
    const bool replaced = qsnapper::security::replaceRegularFileAt(
        parentFd, leafName, srcFd, tryReflink, mustPreserveMetadata, &metadata);
    const int replaceErrno = errno;
    ::close(parentFd);
    ::close(srcFd);

    if (metadata.mandatoryFailed) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to preserve owner, mode, ACL or capability:"
                   << strerror(metadata.mandatoryErrno);
    }
    if (metadata.optionalFailures > 0) {
        qWarning() << "copyRegularFileBeneathRoot:" << metadata.optionalFailures
                   << "extended attribute(s) could not be copied (non-fatal)";
    }
    if (!replaced) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to replace destination:"
                   << strerror(replaceErrno);
        errno = replaceErrno;
        return false;
    }

    return true;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、symlinkをコピーして差し替える
 *
 * 一時symlinkを同じ親dirfd上に作成し、所有者と時刻をfd経由で設定してから、renameat()で差し替える
 * 所有者と時刻の適用失敗は従来どおり非致命とする
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @return symlinkを作成して差し替えできた場合true
 */
bool SnapshotOperations::copySymlinkBeneathRoot(int sourceDirFd,
                                                const QString &sourceRelativePath,
                                                const QString &rootPath,
                                                const QString &dst)
{
    QByteArray linkTarget;
    struct stat srcStat;
    if (!qsnapper::security::safeReadLinkNoFollowAt(sourceDirFd, sourceRelativePath, &linkTarget)
            || !qsnapper::security::safeLstatAt(sourceDirFd, sourceRelativePath, &srcStat)) {
        qWarning() << "copySymlinkBeneathRoot: Failed to read source link:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "copySymlinkBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        return false;
    }

    bool metadataApplied = false;
    const bool replaced = qsnapper::security::replaceSymlinkAt(
        parentFd, leafName, linkTarget, srcStat, &metadataApplied);
    const int replaceErrno = errno;
    ::close(parentFd);

    if (!replaced) {
        qWarning() << "copySymlinkBeneathRoot: Failed to replace destination:"
                   << strerror(replaceErrno);
        errno = replaceErrno;
        return false;
    }
    if (!metadataApplied) {
        qWarning() << "copySymlinkBeneathRoot: Failed to preserve link owner or times (non-fatal)";
    }

    return true;
}
