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
#include "filesystemhelpers_internal.h"
#include <cstring>
#include <dirent.h>
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
 *
 * 退避先の名前は乱数の接尾辞で予測できないため、名前の衝突は通常起きない
 * 衝突した場合 (EEXIST) だけ別の名前で再試行し、上限に達したら何も移動せずに失敗する
 * 1つの親fdで退避し、root実行時の非信頼チェーンでは他者所有のディレクトリをEPERMで拒否する
 * rename前後のdev/ino照合は差し替えを検出して失敗させるが、inodeへの原子的な束縛ではない
 * 不一致時の戻しも名前によるRENAME_NOREPLACEであり、戻せなければ両方の名前を記録する
 *
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

    using namespace qsnapper::security;
    QStringList components;
    if (!splitDestinationComponents(rootPath, path, &components)) {
        return false;
    }
    QByteArray leaf;
    DirectoryChainOwnership ownership;
    const UniqueFd parentFd(openLeafParentBeneathRoot(
        rootPath, components, false, 0, &leaf, &ownership));
    if (!parentFd.isValid()) {
        return errno == ENOENT;
    }
    struct stat before;
    if (::fstatat(parentFd.get(), leaf.constData(), &before, AT_SYMLINK_NOFOLLOW) < 0) {
        return errno == ENOENT;
    }
    const bool guarded = ::geteuid() == 0 && !ownership.trusted;
    if (S_ISDIR(before.st_mode) && guarded
            && (!ownership.principalKnown() || before.st_uid != ownership.principal)) {
        errno = EPERM;
        return false;
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = qsnapper::internal::siblingTemporaryPath(
            path, QStringLiteral("qsnapper-old"));
        const QByteArray detached = candidate.mid(candidate.lastIndexOf(QLatin1Char('/')) + 1).toUtf8();
        if (::renameat2(parentFd.get(), leaf.constData(), parentFd.get(), detached.constData(),
                        RENAME_NOREPLACE) == 0) {
            struct stat after;
            const bool inspected = ::fstatat(parentFd.get(), detached.constData(), &after,
                                             AT_SYMLINK_NOFOLLOW) == 0;
            if (!inspected || before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
                const int error = inspected ? ESTALE : errno;
                if (::renameat2(parentFd.get(), detached.constData(), parentFd.get(), leaf.constData(),
                                RENAME_NOREPLACE) < 0) {
                    qCritical() << "Staged restore: Failed to return mismatched moved path:"
                                << candidate << "to:" << path;
                }
                errno = error;
                return false;
            }
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
 *
 * 復元元がFIFO・ソケット・デバイスファイルのentryは、live側に触れずにスキップする
 * これらは中身を持たず、作り直すとデバイス番号の付いたノードをrootが作成することになるため、復元の対象にしない
 * 退避物の削除失敗もFailedとし、保存場所を記録する
 * 復元失敗後のrename-backは名前による操作でinodeへの束縛ではなく、戻せなければ退避先を記録する
 *
 * @param manifestId 実行contextを選択するマニフェストID
 * @param entry 適用対象entry
 * @return Applied = 適用した、Skipped = 復元できない種別のため触れずにスキップした、Failed = 失敗した
 */
qsnapper::restore::RestoreEntryOutcome SnapshotOperations::applyRestoreEntry(
    const QString &manifestId,
    const qsnapper::restore::RestoreEntry &entry)
{
    using qsnapper::restore::RestoreEntryOutcome;

    const auto execution = m_restoreExecutions.constFind(manifestId);
    if (execution == m_restoreExecutions.cend()) {
        qWarning() << "Staged restore: Missing execution context";
        return RestoreEntryOutcome::Failed;
    }
    const RestoreExecution context = execution.value();
    if (context.snapshotDirFd < 0) {
        qWarning() << "Staged restore: Snapshot source fd is not pinned";
        return RestoreEntryOutcome::Failed;
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
        return RestoreEntryOutcome::Failed;
    }

    const QString snapshotFilePath = context.snapshotDir + QLatin1Char('/') + relativePath;
    if (!qsnapper::security::isPathWithinSnapshotRoot(
            snapshotFilePath, context.snapshotDir)) {
        qWarning() << "Staged restore: Source escaped snapshot root";
        return RestoreEntryOutcome::Failed;
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
            return RestoreEntryOutcome::Failed;
        }

        const bool removed = qsnapper::security::safeRemoveAllBeneathRoot(rootPath, destinationPath);
        if (!removed) {
            const int error = errno;
            qWarning() << "Staged restore: Failed to remove live path:"
                       << strerror(error);
            errno = error;
        }
        return removed ? RestoreEntryOutcome::Applied : RestoreEntryOutcome::Failed;
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
    const bool sourceIsSpecial = hasSnapshotFileInfo
            && (S_ISFIFO(snapshotFileInfo.st_mode) || S_ISSOCK(snapshotFileInfo.st_mode)
                || S_ISCHR(snapshotFileInfo.st_mode) || S_ISBLK(snapshotFileInfo.st_mode));
    if (sourceIsSpecial) {
        // 計画全体を失敗させず、live側の同名objectにも触れない
        // スキップしたpathは計画の終了状態に記録され、GUIに一覧で表示される
        qWarning() << "Staged restore: Skipped a special file that is not restored";
        return RestoreEntryOutcome::Skipped;
    }
    if (!sourceIsLink && !sourceIsDirectory && !sourceIsRegular) {
        qWarning() << "Staged restore: Source is not restorable";
        return RestoreEntryOutcome::Failed;
    }

    // 復元対象ではない欠けた親ディレクトリは、snapshot側の同じディレクトリに合わせて作成する
    // snapshot側に無い場合は0700・root所有のままにし、既存の親ディレクトリは変更しない
    const int slashIndex = destinationPath.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = slashIndex <= 0 ? QStringLiteral("/")
                                               : destinationPath.left(slashIndex);
    if (parentPath != rootPath
            && !qsnapper::security::safeCreateParentDirectoriesFromSourceBeneathRoot(
                rootPath, parentPath, context.snapshotDirFd, ::geteuid() == 0)) {
        // qWarningのstream操作がerrnoを上書きするため、失敗原因を呼び出し元へ返す前に退避・復元する
        const int createError = errno;
        qWarning() << "Staged restore: Failed to create live parent directory:"
                   << strerror(createError);
        errno = createError;
        return RestoreEntryOutcome::Failed;
    }

    // 種別が変わったentryは、既存のobjectを退避してから作り直す
    // ディレクトリとファイルの間はrenameで置き換えられず、既存のディレクトリにmetadataだけを当てると別種別のまま残るため、
    // 復元方式 (YaST / ダイレクト) に関係なく同じ手順にする (libsnapperのundochangeも削除してから作成する)
    QString detachedPath;
    if (entry.changeType == QStringLiteral("typechanged")) {
        if (!movePathAsideBeneathRoot(rootPath, destinationPath, &detachedPath)) {
            const int error = errno;
            qWarning() << "Staged restore: Failed to move live path aside:"
                       << strerror(error);
            errno = error;
            return RestoreEntryOutcome::Failed;
        }
    }

    bool applied = false;
    if (sourceIsLink) {
        applied = copySymlinkBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath);
    }
    else if (sourceIsDirectory) {
        PendingDirectoryTimes pendingTimes;
        applied = applyDirectoryBeneathRoot(context.snapshotDirFd, relativePath, rootPath,
                                            destinationPath, &pendingTimes);
        if (applied && !pendingTimes.relativePath.isEmpty()) {
            m_restoreExecutions[manifestId].pendingDirectoryTimes.append(pendingTimes);
        }
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
                const int error = errno;
                qCritical() << "Staged restore: Failed to remove detached live path."
                            << "Previous content is preserved at:" << detachedPath << strerror(error);
                errno = error;
                return RestoreEntryOutcome::Failed;
            }
        }
        else if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(rootPath, detachedPath, destinationPath)) {
            const int error = errno;
            qCritical() << "Staged restore: Failed to reattach live path after a failed"
                        << "restore. Previous content is preserved at:" << detachedPath;
            errno = error;
        }
    }

    return applied ? RestoreEntryOutcome::Applied : RestoreEntryOutcome::Failed;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、ディレクトリを作成してmetadataを適用する
 *
 * 作成 (mkdirat) と、metadataを適用するためのopenを同じ親dirfd上で行い、名前を再解決しない
 * 既存のディレクトリはそのまま使い、所有者・mode・ACL (access / default)・xattrをsnapshot側に合わせる
 *
 * 書き込み可能な親ディレクトリでは、mkdiratからopenatまでの間に名前を別のディレクトリへ差し替えられ得る
 * 差し替えられたディレクトリにrootが所有者やmodeを適用しないよう、mkdiratの前後とopenatの後でst_devとst_inoを照合し、
 * 一致しなければerrno=ESTALEで失敗する
 * マウントポイント (バインドマウントを含む) やネストしたサブボリュームの根は、別のファイルシステムの根であるため、metadataを適用しない
 *
 * root実行時は、基点から親までの全祖先が信頼できるか、非信頼点以降を単一principalが所有する必要がある
 * 非信頼チェーンでは作成時だけfilesystem identityをprincipalに落とし、作成直後からその所有物にする
 * inodeの一致や空であることは最初の観測前の差し替えを検出できないため、開いた対象とsourceの所有者もprincipalに限る
 * root共有親や他者のディレクトリを経由する変更はEPERMで失敗させる
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @param pendingTimes 全entryの適用後に設定する時刻の返却先 (省略可)
 *                     metadataを適用しなかった場合はrelativePathが空のままになる
 * @return ディレクトリと必須metadataを適用できた場合true
 */
bool SnapshotOperations::applyDirectoryBeneathRoot(int sourceDirFd,
                                                   const QString &sourceRelativePath,
                                                   const QString &rootPath,
                                                   const QString &dst,
                                                   PendingDirectoryTimes *pendingTimes)
{
    const int srcFd = qsnapper::security::safeOpenDirectoryReadAt(sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        const int error = errno;
        qWarning() << "applyDirectoryBeneathRoot: Failed to open source directory:"
                   << strerror(error);
        errno = error;
        return false;
    }

    QByteArray leafName;
    qsnapper::security::DirectoryChainOwnership ownership;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName, &ownership);
    if (parentFd < 0) {
        const int error = errno;
        qWarning() << "applyDirectoryBeneathRoot: Failed to open destination parent:"
                   << strerror(error);
        ::close(srcFd);
        errno = error;
        return false;
    }

    // 失敗時に開いているfdを閉じ、原因のerrnoを記録して返す
    const auto failWith = [&](const char *reason, int error) {
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "applyDirectoryBeneathRoot:" << reason << strerror(error);
        errno = error;
        return false;
    };

    struct stat parentStat;
    if (::fstat(parentFd, &parentStat) < 0) {
        return failWith("Failed to inspect destination parent:", errno);
    }

    struct stat sourceStat;
    if (::fstat(srcFd, &sourceStat) < 0) {
        return failWith("Failed to inspect source directory:", errno);
    }

    // 親単独ではなく最初の非信頼点以降の全祖先を検証し、他者へのroot権限の増幅を防ぐ
    const bool parentUntrusted = ::geteuid() == 0 && !ownership.trusted;
    if (parentUntrusted && (!ownership.principalKnown() || sourceStat.st_uid != ownership.principal)) {
        return failWith("Source owner differs from directory chain principal:", EPERM);
    }

    int mkdirResult;
    if (parentUntrusted) {
        bool identityOk = false;
        int mkdirError = EPERM;
        // fdの解放とログ出力は権限の復旧後に行い、復旧処理からmkdirのerrnoを保護する
        {
            qsnapper::security::ScopedFsIdentity identity(ownership.principal, sourceStat.st_gid);
            identityOk = identity.ok();
            if (identityOk) {
                mkdirResult = ::mkdirat(parentFd, leafName.constData(), 0700);
                mkdirError = errno;
            }
        }
        errno = mkdirError;
        if (!identityOk) {
            return failWith("Failed to assume directory chain principal:", mkdirError);
        }
    }
    else {
        mkdirResult = ::mkdirat(parentFd, leafName.constData(), 0700);
    }

    // 初回観測前の差し替えもあり得るため、作成成功時でも所有者と後続のdev/inoを照合する
    struct stat expected;
    if (mkdirResult == 0) {
        if (::fstatat(parentFd, leafName.constData(), &expected, AT_SYMLINK_NOFOLLOW) < 0) {
            return failWith("Failed to inspect created live directory:", errno);
        }
        const uid_t expectedUid = parentUntrusted ? ownership.principal : ::geteuid();
        if (!S_ISDIR(expected.st_mode) || expected.st_uid != expectedUid) {
            return failWith("Live directory was replaced after creation:", ESTALE);
        }
    }
    else if (errno == EEXIST) {
        if (::fstatat(parentFd, leafName.constData(), &expected, AT_SYMLINK_NOFOLLOW) < 0) {
            return failWith("Failed to inspect existing live directory:", errno);
        }
        if (!S_ISDIR(expected.st_mode)) {
            return failWith("Existing live path is not a directory:", ENOTDIR);
        }
    }
    else {
        return failWith("Failed to create live directory:", errno);
    }

    const int directoryFd = ::openat(parentFd, leafName.constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) {
        return failWith("Failed to open live directory:", errno);
    }
    ::close(parentFd);

    struct stat opened;
    int openedError = 0;
    if (::fstat(directoryFd, &opened) < 0) {
        openedError = errno;
    }
    else if (opened.st_dev != expected.st_dev || opened.st_ino != expected.st_ino) {
        openedError = ESTALE;
    }
    if (openedError != 0) {
        const int error = openedError;
        ::close(directoryFd);
        ::close(srcFd);
        qWarning() << "applyDirectoryBeneathRoot: Live directory was replaced before opening:"
                   << strerror(error);
        errno = error;
        return false;
    }

    // st_devが同じバインドマウントも検出できるよう、STATX_ATTR_MOUNT_ROOTを使って境界を判定する
    struct statx openedInfo;
    if (::statx(directoryFd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO, &openedInfo) != 0) {
        const int error = errno;
        ::close(directoryFd);
        ::close(srcFd);
        qWarning() << "applyDirectoryBeneathRoot: Failed to inspect live directory:" << strerror(error);
        errno = error;
        return false;
    }

    if (qsnapper::security::isMountBoundary(openedInfo, parentStat.st_dev)) {
        // マウントポイントやネストしたサブボリュームの根は、その先のファイルシステムの所有者とmodeを持つため変更しない
        ::close(directoryFd);
        ::close(srcFd);
        qWarning() << "applyDirectoryBeneathRoot: Skipped metadata of a mount point or nested subvolume";
        return true;
    }

    if (parentUntrusted) {
        // 復元元の所有者は作成前に照合済みのため、ここでは新しく開いたinodeの所有者だけを確認する
        if (opened.st_uid != ownership.principal) {
            // 入れ替えられた別のディレクトリへrootが所有者やmodeを適用しないよう、復元を失敗させて管理者に知らせる
            ::close(directoryFd);
            ::close(srcFd);
            qWarning() << "applyDirectoryBeneathRoot: Refused metadata outside directory chain principal";
            errno = EPERM;
            return false;
        }
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

    if (pendingTimes) {
        // snapshot dirfdからの相対パスは、rootPathからの相対パスと同じ形で組み立てられている
        pendingTimes->rootPath = rootPath;
        pendingTimes->relativePath = sourceRelativePath;
        pendingTimes->device = static_cast<quint64>(opened.st_dev);
        pendingTimes->inode = static_cast<quint64>(opened.st_ino);
        pendingTimes->accessSec = static_cast<qint64>(sourceStat.st_atim.tv_sec);
        pendingTimes->accessNsec = static_cast<qint64>(sourceStat.st_atim.tv_nsec);
        pendingTimes->modifySec = static_cast<qint64>(sourceStat.st_mtim.tv_sec);
        pendingTimes->modifyNsec = static_cast<qint64>(sourceStat.st_mtim.tv_nsec);
    }

    return true;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、通常ファイルをコピーして差し替える
 *
 * live側を直接O_TRUNCで開くと、実行中のバイナリ (復元を実行しているqSnapper自身や稼働中のサービス) がETXTBSYで拒否され、既存inodeのhardlinkや緩いmodeも引き継いでしまう
 * そのため、同じ親dirfd上の一時ファイルへ書き出し、data → 所有者 → ACL → mode → xattr → capability → 時刻をfd経由で適用してから、renameat()で差し替える
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
    qsnapper::security::DirectoryChainOwnership ownership;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName, &ownership);
    if (parentFd < 0) {
        const int error = errno;
        qWarning() << "copyRegularFileBeneathRoot: Failed to open destination parent:"
                   << strerror(error);
        ::close(srcFd);
        errno = error;
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
 * rootで動作する場合、所有者と時刻の適用失敗は失敗として扱い、live側を差し替えない
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
    qsnapper::security::DirectoryChainOwnership ownership;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName, &ownership);
    if (parentFd < 0) {
        const int error = errno;
        qWarning() << "copySymlinkBeneathRoot: Failed to open destination parent:"
                   << strerror(error);
        errno = error;
        return false;
    }

    // rootで動作する本番環境では、通常ファイルと同じく所有者と時刻を保てない復元を失敗扱いにする
    // その場合はrenameせずに一時symlinkを片付けるため、live側は元のまま残る
    const bool mustPreserveMetadata = (::geteuid() == 0);
    bool metadataApplied = false;
    const bool replaced = qsnapper::security::replaceSymlinkAt(
        parentFd, leafName, linkTarget, srcStat, &metadataApplied, mustPreserveMetadata);
    const int replaceErrno = errno;
    ::close(parentFd);

    if (!replaced) {
        qWarning() << "copySymlinkBeneathRoot: Failed to replace destination:"
                   << strerror(replaceErrno);
        errno = replaceErrno;
        return false;
    }
    if (!metadataApplied) {
        // root以外 (テストや開発時の実行) では所有者を変更できないため、非致命として続行する
        qWarning() << "copySymlinkBeneathRoot: Failed to preserve link owner or times (non-fatal)";
    }

    return true;
}

/**
 * @brief 全entryの適用後に、適用したディレクトリの時刻を復元元に合わせる
 *
 * 子entryの作成・削除で親ディレクトリのmtimeが変わるため、ディレクトリの適用時ではなく計画の終了時にまとめて設定する
 * 適用時から別のディレクトリへ差し替えられていた場合は、他者のディレクトリの時刻を変えないよう設定しない
 * 時刻は所有者やmodeと違ってアクセス制御に影響しないため、設定できなくても計画の結果は変えずに警告だけを残す
 *
 * @param pendingTimes 適用したディレクトリと設定する時刻の一覧
 * @return 設定できなかったディレクトリの数
 */
int SnapshotOperations::applyPendingDirectoryTimes(const QList<PendingDirectoryTimes> &pendingTimes)
{
    int failures = 0;
    for (const PendingDirectoryTimes &pending : pendingTimes) {
        const int directoryFd = qsnapper::security::safeOpenDirectoryBeneathRoot(
            pending.rootPath, pending.relativePath, false, 0);
        // 開けなかったディレクトリは、差し替えられたか削除された可能性があるため、時刻を設定せずに失敗として数える
        if (directoryFd < 0) {
            ++failures;
            continue;
        }

        struct stat current;
        const bool sameDirectory = ::fstat(directoryFd, &current) == 0
                && static_cast<quint64>(current.st_dev) == pending.device
                && static_cast<quint64>(current.st_ino) == pending.inode;
        struct timespec times[2];
        times[0].tv_sec = static_cast<time_t>(pending.accessSec);
        times[0].tv_nsec = static_cast<long>(pending.accessNsec);
        times[1].tv_sec = static_cast<time_t>(pending.modifySec);
        times[1].tv_nsec = static_cast<long>(pending.modifyNsec);
        if (!sameDirectory || ::futimens(directoryFd, times) < 0) {
            ++failures;
        }
        ::close(directoryFd);
    }

    if (failures > 0) {
        qWarning() << "Staged restore:" << failures
                   << "directory time stamp(s) could not be restored (non-fatal)";
    }
    return failures;
}
