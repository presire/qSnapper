/**
 * @file snapshotoperations_restore.cpp
 * @brief staged復元計画の開始と確定
 *
 * BeginRestorePlanとStageRestoreEntries、CommitRestorePlan
 * 凍結計画と復元元・比較相手の木との照合、ContinueとStatusとCancelを扱う
 * 計画の作成時に差分の閲覧と同じview-diffで認可し、復元の実行はCommitRestorePlanの中で計画凍結の後に1回だけ認可する
 */

#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDateTime>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include "restorevalidation.h"
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/Exception.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * @brief ownerに束縛された空のstaged restore計画を開始する
 *
 * 引数と呼び出し元を検証した後、view-diffの認可を経てbeginRestorePlanAuthorizedで計画を作る
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元snapshot番号
 * @param counterpartSnapshotNumber 比較相手のsnapshot番号 (0は現在のシステム)
 * @param restoreMode yastまたはdirect
 * @return 即時に認可された場合はマニフェストID、遅延応答や失敗の場合は空文字列
 */
QString SnapshotOperations::BeginRestorePlan(const QString &configName,
                                             int snapshotNumber,
                                             int counterpartSnapshotNumber,
                                             const QString &restoreMode)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return {};
    }

    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return {};
    }

    qsnapper::restore::RestoreMode mode;
    if (restoreMode == QStringLiteral("yast")) {
        mode = qsnapper::restore::RestoreMode::YastCompatible;
    }
    else if (restoreMode == QStringLiteral("direct")) {
        mode = qsnapper::restore::RestoreMode::DirectCopy;
    }
    else {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid restore mode"));
        return {};
    }

    if (snapshotNumber <= 0) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid snapshot number"));
        return {};
    }

    // 0は「現在のシステム」のsentinelである (snapshot vs currentのbulk経路が使用する)
    // 正の値は復元元と別のsnapshotでなければならない
    if (counterpartSnapshotNumber < 0
            || (counterpartSnapshotNumber > 0
                && counterpartSnapshotNumber == snapshotNumber)) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid counterpart snapshot number"));
        return {};
    }

    // 計画の予算はUID単位でも数えるため、呼び出し元のUIDをバスに問い合わせる
    // 取得できない呼び出し元には計画を作らせない
    uint ownerUid = 0;
    if (calledFromDBus()) {
        const QDBusConnectionInterface *bus = connection().interface();
        const QDBusReply<uint> uidReply =
            bus ? bus->serviceUid(owner) : QDBusReply<uint>();
        if (!uidReply.isValid()) {
            replyError(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return {};
        }
        ownerUid = uidReply.value();
    }
    else {
        // D-Busを介さない直接呼び出し (単体テスト) は、自プロセスの実効UIDとして数える
        ownerUid = static_cast<uint>(::geteuid());
    }

    // 計画の作成には、差分の閲覧と同じview-diffの認可を求める
    // 計画は件数とパスの予算を消費するため、認可のない任意のローカルユーザが枠を使い切り、管理者の復元を妨げるのを防ぐ
    // GUIは差分を表示するときに同じ認可を得ており、auth_admin_keepのキャッシュが効くため、通常は追加のプロンプトが出ない
    // 計画を作るだけでは復元を実行しないため、実行中の復元があっても受け付ける (認可を求める前と同じ挙動)
    const QString resolvedConfigName = *cfg;
    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, owner, ownerUid, resolvedConfigName, snapshotNumber, counterpartSnapshotNumber, mode]() {
            return beginRestorePlanAuthorized(owner, ownerUid, resolvedConfigName, snapshotNumber,
                                              counterpartSnapshotNumber, mode);
        },
        RestoreGatePolicy::AllowWhileRestoring);
}

/**
 * @brief 認可済みのBeginRestorePlan本体として、同じUIDの古い計画を整理してから空の計画を作る
 *
 * 同じUIDの計画数が上限に達している場合は、そのUIDの最も古いStaging計画から破棄して枠を空ける
 * GUIの不具合や同じUIDの別プロセスが空の計画を作り続けても、利用者が新しい復元を始められなくなるのを防ぐため
 * 破棄するのは呼び出し元と同じUIDのStaging計画だけで、凍結済み・実行中の計画や他のユーザの計画には触れない
 *
 * @param owner 計画を束縛するD-Bus接続名 (直接呼び出しでは空文字列)
 * @param ownerUid 計画の予算を数えるUID
 * @param configName 検証済みのSnapper設定名
 * @param snapshotNumber 復元元snapshot番号
 * @param counterpartSnapshotNumber 比較相手のsnapshot番号 (0は現在のシステム)
 * @param mode 検証済みの復元モード
 * @return 成功時マニフェストID、失敗時はエラー応答済みで空文字列
 * @note 破棄した計画はrestorePlanFinishedでcancelledとして通知し、元のownerは終端状態を取得できる
 */
QString SnapshotOperations::beginRestorePlanAuthorized(const QString &owner,
                                                       uint ownerUid,
                                                       const QString &configName,
                                                       int snapshotNumber,
                                                       int counterpartSnapshotNumber,
                                                       qsnapper::restore::RestoreMode mode)
{
    using qsnapper::restore::RestoreManifestRegistry;

    purgeExpiredRestorePlans();

    // 上限はownerとUIDの両方にあるが、同じownerの計画は必ず同じUIDに属し、上限の値も等しい
    // そのためownerが上限に達しているときは、UIDの計画はすべてそのownerのものであり、古い順の破棄で両方の枠が空く
    const QStringList evictable = m_restoreRegistry.stagingIdsForUidOldestFirst(ownerUid, QString());
    qsizetype evictIndex = 0;
    while (evictIndex < evictable.size()
           && (m_restoreRegistry.countForUid(ownerUid) >= RestoreManifestRegistry::kMaxManifestsPerUid
               || m_restoreRegistry.countForOwner(owner) >= RestoreManifestRegistry::kMaxManifestsPerOwner)) {
        const QString &evictedId = evictable.at(evictIndex);
        ++evictIndex;

        // 破棄する計画は別の接続が作った可能性があるため、registryにはその計画のownerとして取り消しを伝える
        qsnapper::restore::ManifestError cancelError = qsnapper::restore::ManifestError::None;
        const QString evictedOwner = m_restorePlanOwners.value(evictedId);
        if (!m_restoreRegistry.cancel(evictedId, evictedOwner, &cancelError)) {
            continue;
        }
        finishRestorePlan(evictedId, qsnapper::restore::ManifestState::Cancelled,
                          QStringLiteral("Restore plan was replaced by a newer plan"));
    }

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const QString manifestId = m_restoreRegistry.createStaging(
        owner, ownerUid, configName, snapshotNumber, counterpartSnapshotNumber, mode,
        &error);
    if (manifestId.isEmpty()) {
        sendManifestError(error);
        return {};
    }

    m_restorePlanOwners.insert(manifestId, owner);
    if (m_ownerWatcher && !owner.isEmpty()
            && !m_ownerWatcher->watchedServices().contains(owner)) {
        m_ownerWatcher->addWatchedService(owner);
    }
    return manifestId;
}

/**
 * @brief staging計画へ検証済みentry chunkを原子的に追加する
 * @param manifestId 呼び出し元のD-Bus接続 (owner) に結び付けられたマニフェストID
 * @param filePaths 復元対象絶対path列
 * @param changeTypes pathと対応する変更種別列
 * @return chunk全体を追加できた場合true
 */
bool SnapshotOperations::StageRestoreEntries(
    const QString &manifestId,
    const QStringList &filePaths,
    const QStringList &changeTypes)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    // エントリの追加は復元の実行の認可を要さず、グローバル予算を消費する唯一の経路でもある
    // 失効済み計画をここで回収しておかないと、放置された計画が予算を占有し続け、正規の利用者がGlobalLimitで弾かれる
    purgeExpiredRestorePlans();

    if (filePaths.size() != changeTypes.size()) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Restore entry lists must have the same size"));
        return false;
    }
    if (filePaths.isEmpty()) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Restore entry chunk is empty"));
        return false;
    }
    if (filePaths.size()
            > qsnapper::restore::RestoreManifestRegistry::kMaxEntriesPerStageChunk) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Restore entry chunk is too large"));
        return false;
    }

    for (qsizetype index = 0; index < filePaths.size(); ++index) {
        const QString &path = filePaths.at(index);
        const QString &changeType = changeTypes.at(index);
        const bool validChangeType = changeType == QStringLiteral("created")
                || changeType == QStringLiteral("deleted")
                || changeType == QStringLiteral("modified")
                || changeType == QStringLiteral("typechanged");
        // pathはconfig相対名であるため、SUBVOLUMEに依存しない"/"を仮の基準として形式だけを検証する
        // /.snapshotsの判定は正規化後の先頭成分で行う ("//.snapshots/..."等の迂回を防ぐ)
        QString rootPath;
        QString destinationPath;
        QString relativePath;
        if (!validChangeType
                || qsnapper::restore::isSnapshotMetadataRestoreName(path)
                || !qsnapper::restore::buildRestoreDestination(
                    QStringLiteral("/"), path, &rootPath, &destinationPath, &relativePath)) {
            replyError(QDBusError::InvalidArgs,
                           QStringLiteral("Invalid restore entry"));
            return false;
        }
    }

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    if (!m_restoreRegistry.stageEntries(manifestId, owner, filePaths,
                                        changeTypes, &error)) {
        return sendManifestError(error);
    }
    return true;
}

/**
 * @brief 計画をfreeze後に1度だけ認可し非同期実行を開始する
 * @param manifestId 呼び出し元のD-Bus接続 (owner) に結び付けられたマニフェストID
 * @return 実行開始を受理した場合true
 */
bool SnapshotOperations::CommitRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    // 復元はlive filesystemを書き換える排他的な操作である
    // 複数計画がchunk境界で交互実行されると最終状態が非決定になり、
    // libsnapperのmount_user_requestがbool (カウンタではない) であることも相まってmount管理が衝突し得るため、同時に1計画のみ実行を許す
    if (!m_restoreExecutions.isEmpty()) {
        replyError(QDBusError::Failed,
                       QStringLiteral("Another restore plan is already running"));
        return false;
    }

    // systemdへ送信済みのtimerの変更は取り消せないため、その完了を待たずに復元を始めない
    // 計画を凍結する前に拒否し、timerの操作が終わった後に同じ計画で再びcommitできるようにする
    if (timerChangeInProgress()) {
        replyError(QDBusError::Failed,
                   QStringLiteral("A timer operation is in progress"));
        return false;
    }

    purgeExpiredRestorePlans();

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const auto status = m_restoreRegistry.status(manifestId, owner, &error);
    if (!status) {
        return sendManifestError(error);
    }
    if (status->state == qsnapper::restore::ManifestState::Completed
            || status->state == qsnapper::restore::ManifestState::Failed
            || status->state == qsnapper::restore::ManifestState::Cancelled) {
        return sendManifestError(
            qsnapper::restore::ManifestError::AlreadyTerminal);
    }
    if (status->state != qsnapper::restore::ManifestState::Staging) {
        return sendManifestError(qsnapper::restore::ManifestError::WrongState);
    }
    if (status->totalEntries <= 0) {
        return sendManifestError(
            qsnapper::restore::ManifestError::InvalidArgument);
    }
    if (!m_restoreRegistry.freeze(manifestId, owner, &error)) {
        return sendManifestError(error);
    }

    // 認可対象は凍結済みの不変計画である
    // 認可待ちの間にcancel / TTL失効 / owner消失 / 他計画の実行開始が起こり得るため、実際のmountとexecutor起動は継続側で状態を再検証してから行う
    const AuthorizationOutcome outcome = beginAuthorization(
        QStringLiteral("com.presire.qsnapper.rollback-snapshot"),
        [this, manifestId, owner](const CallReply &reply, bool granted) {
            m_deferredReply = reply;
            bool accepted = false;
            if (granted) {
                accepted = commitRestorePlanAuthorized(manifestId, owner);
            }
            else {
                failRestorePlanAuthorization(manifestId, owner);
                replyError(QDBusError::AccessDenied,
                           QStringLiteral("Authorization failed"));
            }

            const bool alreadyReplied = m_deferredReply->replied;
            m_deferredReply.reset();
            if (!alreadyReplied) {
                reply.connection.send(
                    reply.message.createReply(QVariant::fromValue(accepted)));
            }
        });

    switch (outcome) {
    case AuthorizationOutcome::Granted:
        return commitRestorePlanAuthorized(manifestId, owner);
    case AuthorizationOutcome::Denied:
        // beginAuthorizationがD-Busエラー応答を送出済み
        failRestorePlanAuthorization(manifestId, owner);
        return false;
    case AuthorizationOutcome::Deferred:
        break;
    }
    return false;
}

/**
 * @brief 認可が得られなかった復元計画をFailedで終端する
 * @param manifestId 対象マニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 */
void SnapshotOperations::failRestorePlanAuthorization(const QString &manifestId,
                                                      const QString &owner)
{
    qsnapper::restore::ManifestError failureError =
        qsnapper::restore::ManifestError::None;
    m_restoreRegistry.markFailed(
        manifestId, owner, QStringLiteral("Authorization failed"),
        &failureError);
}

/**
 * @brief 認可済みのCommitRestorePlan本体
 *
 * 認可待ちの間にevent loopが回るため、凍結済み計画が生き残っている保証はない
 * mountやexecutor起動といった不可逆な操作の前に、以下を必ず再検証する:
 *   - 他の計画が実行を開始していないこと (復元はlive filesystemへの排他操作)
 *   - 計画がownerに束縛されたまま存在し、まだFrozenであること
 *     (cancel / TTL失効 / owner消失は全てここで弾かれる)
 *   - 凍結済みの全entryが、復元元と比較相手の木の実際の差分に裏付けられていること
 *     (クライアント申告のchangeTypeもlibsnapperの比較結果も信頼しない)
 *
 * @param manifestId ownerに束縛されたマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @return 実行開始を受理した場合true
 */
bool SnapshotOperations::commitRestorePlanAuthorized(const QString &manifestId,
                                                     const QString &owner)
{
    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;

    // 認可待ちの間に別の計画がcommitされている可能性がある
    if (!m_restoreExecutions.isEmpty()) {
        failRestorePlanAuthorization(manifestId, owner);
        replyError(QDBusError::Failed,
                   QStringLiteral("Another restore plan is already running"));
        return false;
    }

    // 認可待ちの間にtimerの変更が始まっている可能性がある
    // mountと実行の登録より前に拒否し、送信済みのsystemdの変更と復元が重ならないようにする
    if (timerChangeInProgress()) {
        failRestorePlanAuthorization(manifestId, owner);
        replyError(QDBusError::Failed,
                   QStringLiteral("A timer operation is in progress"));
        return false;
    }

    // 認可待ちの間のcancel / TTL失効 / owner消失 / purgeを検出する
    const auto status = m_restoreRegistry.status(manifestId, owner, &error);
    if (!status) {
        return sendManifestError(error);
    }
    if (status->state != qsnapper::restore::ManifestState::Frozen) {
        return sendManifestError(qsnapper::restore::ManifestError::WrongState);
    }

    RestoreExecution execution;
    execution.owner = owner;
    execution.configName = status->configName;
    execution.snapshotNumber = status->snapshotNumber;
    execution.useReflink =
        status->mode == qsnapper::restore::RestoreMode::DirectCopy;

    try {
        // Snapperは構築時にsnapshot一覧をcaptureし外部追加を観測しないため、
        // 比較相手を解決する前に強制再生成する
        // cache clearが先である理由は、古いSnapperを参照するComparisonが
        // cacheへ残るのを防ぐためである
        m_comparisonCache.clear();
        snapper::Snapper *snapper = getSnapper(execution.configName,
                                               /*forceReload=*/true);
        if (!snapper) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Failed to initialize restore source"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        const snapper::Snapshots::const_iterator source =
            snapper->getSnapshots().find(execution.snapshotNumber);
        if (source == snapper->getSnapshots().end()) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore source snapshot is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // 復元元と同じ向きで比較相手を解決する
        // 0は「現在のシステム」のsentinelであり、正の値は番号で解決する
        // (Pre/Postの順序を推測・並べ替えるとCREATEDとDELETEDが反転する)
        snapper::Snapshots::const_iterator counterpart;
        if (status->counterpartSnapshotNumber == 0) {
            counterpart = snapper->getSnapshotCurrent();
        }
        else {
            counterpart = snapper->getSnapshots().find(
                status->counterpartSnapshotNumber);
        }
        if (counterpart == snapper->getSnapshots().end()
                || counterpart == source) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore comparison counterpart is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // 復元先の基準となるconfigのSUBVOLUMEを固定する
        // 計画に載る名前はconfig相対名であり、宛先は"<SUBVOLUME>/<名前>"として実行時に組み立てる
        execution.subvolume = QString::fromStdString(snapper->subvolumeDir());
        QString normalizedSubvolume;
        if (!qsnapper::restore::normalizeRestoreSubvolume(execution.subvolume,
                                                          &normalizedSubvolume)) {
            qWarning() << "Staged restore: Config subvolume is not usable as a restore root";
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore destination is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // root安全ネットの判定材料として、復元前のrootサブボリュームの状態を記録する
        // 元からread-onlyのroot (read-only snapshotからの起動など) を、復元後にrwへ変えないため
        execution.rootReadWriteState.targetsRootSubvolume =
            normalizedSubvolume == QStringLiteral("/");
        if (execution.rootReadWriteState.targetsRootSubvolume && m_rootReadOnlyProbe) {
            bool readOnly = true;
            execution.rootReadWriteState.preRestoreStateKnown =
                m_rootReadOnlyProbe(&readOnly);
            execution.rootReadWriteState.preRestoreReadOnly = readOnly;
        }

        source->mountFilesystemSnapshot(true);
        execution.mounted = true;
        const QString snapshotDir =
            QString::fromStdString(source->snapshotDir());
        m_restoreExecutions.insert(manifestId, execution);
        m_restoreExecutions[manifestId].snapshotDir = snapshotDir;

        // ソースsnapshotをdirfdでpinする
        // 以降のソース読み取りは本fd相対で行うため、chunk境界でevent loopが回る間にpath上のsnapshotが削除 / 同番号で再作成 / 差し替えられても、
        // 認可時に参照したsnapshotそのものから復元し続ける
        const int snapshotDirFd = ::open(snapshotDir.toUtf8().constData(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (snapshotDirFd < 0) {
            qWarning() << "Staged restore: Failed to pin snapshot dir:"
                       << strerror(errno);
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        m_restoreExecutions[manifestId].snapshotDirFd = snapshotDirFd;

        // pin済みfdの同一性判定に使うstatを確定させる
        struct stat pinnedStat = {};
        if (::fstat(snapshotDirFd, &pinnedStat) != 0) {
            qWarning() << "Staged restore: Failed to stat pinned snapshot dir:"
                       << strerror(errno);
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }

        // dirfdのpinで固定されるのはinodeだけで、内容は固定されない
        // 書き込み可能な復元元 (rollbackが作るwritable copyなど) では、認可の後に内容を差し替えられるため拒否する
        // libsnapperのメタデータと、pin済みfdに対するファイルシステムの状態の両方が読み取り専用であることを要求する
        std::string fstype;
        snapper->getConfigInfo().get_value("FSTYPE", fstype);
        if (!source->isReadOnly()
                || !qsnapper::restore::isPinnedRestoreSourceReadOnly(snapshotDirFd, fstype)) {
            qWarning() << "Staged restore: Restore source snapshot is not read-only";
            cleanupRestoreExecution(manifestId);
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore source snapshot is not read-only"),
                &error);
            replyError(QDBusError::Failed,
                       QStringLiteral("Restore source snapshot is not read-only"));
            return false;
        }

        // executor起動前の最終検証
        // 全entryが復元元と比較相手の木の実際の差分に裏付けられ、pin済みdirfdの同一性確認も通るまで、
        // 復元は1バイトもlive filesystemへ書き込まない
        try {
            if (!validateFrozenRestorePlan(manifestId, owner, counterpart,
                                           status->counterpartSnapshotNumber == 0,
                                           snapshotDir, pinnedStat)) {
                failRestorePlanPreflight(manifestId, owner, &error);
                return false;
            }
        }
        catch (const snapper::Exception &) {
            // 例外本文にはlibsnapperが扱ったfilesystem pathが含まれ得るため記録しない
            // (攻撃者由来の名前をログへ持ち込まない)
            qWarning() << "Staged restore: Restore plan validation failed";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (const std::exception &) {
            qWarning() << "Staged restore: Restore plan validation failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (...) {
            qWarning() << "Staged restore: Restore plan validation failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
    }
    catch (const snapper::Exception &) {
        // 例外本文にはlibsnapperが扱ったfilesystem pathが含まれ得るため記録しない
        // (攻撃者由来の名前をログへ持ち込まない)
        qWarning() << "Staged restore preparation failed";
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                        QStringLiteral("Failed to prepare restore plan"));
        return false;
    }
    catch (const std::exception &) {
        qWarning() << "Staged restore preparation failed unexpectedly";
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                        QStringLiteral("Failed to prepare restore plan"));
        return false;
    }
    catch (...) {
        qWarning() << "Staged restore preparation failed unexpectedly";
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                        QStringLiteral("Failed to prepare restore plan"));
        return false;
    }

    // 安全ネットは実行を開始した計画にだけ適用する
    // start()が失敗した場合はcleanupRestoreExecutionで実行contextごと破棄されるため、この印は残らない
    const auto startedExecution = m_restoreExecutions.find(manifestId);
    if (startedExecution != m_restoreExecutions.end()) {
        startedExecution->rootReadWriteState.executionStarted = true;
    }
    if (!m_restoreExecutor.start(manifestId, owner, &error)) {
        cleanupRestoreExecution(manifestId);
        qsnapper::restore::ManifestError failureError =
            qsnapper::restore::ManifestError::None;
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to start restore work"),
            &failureError);
        return sendManifestError(error);
    }

    return true;
}

namespace {

/**
 * @class CounterpartTreeGuard
 * @brief 検証の間だけ比較相手の木を開いておき、例外を含む全ての経路で閉じる
 *
 * 比較相手がスナップショットの場合は、マウントの参照を1つ取得してから開く
 * 参照はuser_request=falseで取り、利用者がGUIから要求したマウント (user_request=true) の状態を変えない
 * 閉じる順序は復元元と同じく、fdを閉じてからマウントの参照を返す (開いたfdがマウントをbusyにするため)
 *
 * m_snapshot : マウントの参照を返す対象 (live側の場合は使わない)
 * m_mounted : マウントの参照を取得済みか
 * m_fd : 比較相手の木の根のdirfd
 */
class CounterpartTreeGuard
{
public:
    /**
     * @brief 比較相手の木を開く
     * @param snapshot 比較相手のスナップショット (counterpartIsCurrentがtrueなら使わない)
     * @param counterpartIsCurrent 比較相手が現在のシステムか
     * @param liveRootPath 正規化済みのSUBVOLUME (現在のシステムの木の根)
     * @note libsnapperのマウントは例外を送出し得るため、呼び出し側で捕捉すること
     */
    CounterpartTreeGuard(const snapper::Snapshots::const_iterator &snapshot,
                         bool counterpartIsCurrent,
                         const QString &liveRootPath)
        : m_snapshot(snapshot)
    {
        if (counterpartIsCurrent) {
            m_fd = qsnapper::security::safeOpenDirectory(liveRootPath);
            return;
        }

        m_snapshot->mountFilesystemSnapshot(false);
        m_mounted = true;
        m_fd = ::open(m_snapshot->snapshotDir().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    /**
     * @brief fdを閉じてからマウントの参照を返す
     */
    ~CounterpartTreeGuard()
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        if (!m_mounted) {
            return;
        }

        // デストラクタから例外を送出しないよう、アンマウントの失敗は記録だけにする
        try {
            m_snapshot->umountFilesystemSnapshot(false);
        }
        catch (...) {
            qWarning() << "Staged restore: Failed to release the comparison counterpart mount";
        }
    }

    CounterpartTreeGuard(const CounterpartTreeGuard &) = delete;
    CounterpartTreeGuard &operator=(const CounterpartTreeGuard &) = delete;

    /**
     * @brief 比較相手の木の根のdirfdを返す
     * @return 開けなかった場合は-1
     */
    int fd() const
    {
        return m_fd;
    }

private:
    snapper::Snapshots::const_iterator m_snapshot; // マウントの参照を返す対象
    bool m_mounted = false;                        // マウントの参照を取得済みか
    int m_fd = -1;                                 // 比較相手の木の根のdirfd
};

/**
 * @brief dirfdを根とする木を観測する関数を作る
 * @param rootFd 観測する木の根のdirfd (関数の利用中は開いたままにすること)
 * @return 相対パスを受け取り観測結果を返す関数
 */
qsnapper::restore::RestorePathObserver makeTreeObserver(int rootFd)
{
    return [rootFd](const QString &relativePath) {
        qsnapper::restore::RestorePathObservation observation;
        struct stat info = {};
        switch (qsnapper::security::observePathNoFollowAt(rootFd, relativePath, &info)) {
        case qsnapper::security::PathPresenceAt::Present:
            observation = qsnapper::restore::restorePathObservationFromStat(info);
            break;
        case qsnapper::security::PathPresenceAt::Absent:
            observation.presence = qsnapper::restore::RestorePathPresence::Absent;
            break;
        case qsnapper::security::PathPresenceAt::Unknown:
            observation.presence = qsnapper::restore::RestorePathPresence::Unknown;
            break;
        }
        return observation;
    };
}

} // namespace

/**
 * @brief 凍結済み計画の全entryを、復元元と比較相手の木の直接観測で検証する
 * @param manifestId ownerに束縛されたマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @param counterpart 比較相手snapshotのiterator (counterpartIsCurrentがtrueなら使わない)
 * @param counterpartIsCurrent 比較相手が現在のシステムか
 * @param snapshotDir pin済みdirfdが指すsnapshot directoryのpath
 * @param pinnedStat pin済みdirfdに対するfstatの結果
 * @return 全entryが実際の差分に裏付けられ、同一性確認も通った場合true
 *
 * @note snapper::Comparisonの結果は照合の基準に使わない
 *       read-only同士の比較ではlibsnapperのfilelistキャッシュが読まれる
 *       このキャッシュは名前をエスケープせずに1行1件で並べた形式であり、改行を含むファイル名を置かれると偽のentryが生まれるため
 *       (詳細はrestorevalidation.hのvalidateFrozenEntriesAgainstTreesを参照)
 *       復元元は認可時にpinしたdirfd、比較相手は本関数で開いたdirfdを根として、entryごとに存在と種別を直接観測する
 *       攻撃者が付けた名前はログにもエラー本文にも載せない
 */
bool SnapshotOperations::validateFrozenRestorePlan(
    const QString &manifestId,
    const QString &owner,
    const snapper::Snapshots::const_iterator &counterpart,
    bool counterpartIsCurrent,
    const QString &snapshotDir,
    const struct stat &pinnedStat)
{
    // 検証は認可時に固定した復元元だけを根にするため、pinが無い計画は実行しない
    const auto execution = m_restoreExecutions.constFind(manifestId);
    if (execution == m_restoreExecutions.cend() || execution->snapshotDirFd < 0) {
        qWarning() << "Staged restore: Missing pinned restore source for validation";
        return false;
    }
    const int sourceFd = execution->snapshotDirFd;
    const QString subvolume = execution->subvolume;

    QString liveRootPath;
    // live側の根を正規化できないと比較相手の木を安全に開けないため、検証を打ち切る
    if (!qsnapper::restore::normalizeRestoreSubvolume(subvolume, &liveRootPath)) {
        return false;
    }

    {
        const CounterpartTreeGuard counterpartTree(counterpart, counterpartIsCurrent,
                                                   liveRootPath);
        // 比較相手を開けないと差分を直接観測できないため、裏付けなしとして実行を拒否する
        if (counterpartTree.fd() < 0) {
            qWarning() << "Staged restore: Failed to open the comparison counterpart:"
                       << strerror(errno);
            return false;
        }

        qsnapper::restore::ManifestError validationError =
            qsnapper::restore::ManifestError::None;
        // 1件でも裏付けのないentryがあれば、計画全体の実行を失敗させる (fail-closed)
        if (!qsnapper::restore::validateFrozenEntriesAgainstTrees(
                m_restoreRegistry, manifestId, owner, subvolume,
                makeTreeObserver(sourceFd), makeTreeObserver(counterpartTree.fd()),
                &validationError)) {
            qWarning() << "Staged restore: Frozen plan contains an entry that is not"
                       << "backed by the restore source and counterpart trees";
            return false;
        }
    }

    // pin済みfdと、pathを改めて開いた結果が同一inodeであることを確認する
    // (st_devとst_inoの両方) 異なっていれば、検証対象と実行対象のスナップショットが入れ替わっているため実行を拒否する
    // 実行は以降もpin済みfdのみを通る
    struct stat currentStat = {};
    if (::stat(snapshotDir.toUtf8().constData(), &currentStat) != 0
            || currentStat.st_dev != pinnedStat.st_dev
            || currentStat.st_ino != pinnedStat.st_ino) {
        qWarning() << "Staged restore: Snapshot directory identity changed"
                   << "during validation";
        return false;
    }
    return true;
}

/**
 * @brief commit時preflight失敗を計画のfail-closed終端へ落とし込む
 * @param manifestId 検証に失敗したマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @param error registryのmarkFailedへ渡すエラー格納先
 */
void SnapshotOperations::failRestorePlanPreflight(
    const QString &manifestId,
    const QString &owner,
    qsnapper::restore::ManifestError *error)
{
    cleanupRestoreExecution(manifestId);
    m_restoreRegistry.markFailed(
        manifestId, owner, QStringLiteral("Restore plan validation failed"),
        error);
    qWarning() << "Staged restore: Restore plan failed commit-time validation";
    replyError(QDBusError::Failed,
               QStringLiteral("Restore plan validation failed"));
}

/**
 * @brief owner確認済みRunning計画のidle loopを再開する
 * @param manifestId 呼び出し元のD-Bus接続 (owner) に結び付けられたマニフェストID
 * @return nudgeを受理した場合: true
 */
bool SnapshotOperations::ContinueRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    if (!m_restoreExecutor.requestContinue(manifestId, owner, &error)) {
        return sendManifestError(error);
    }
    return true;
}

/**
 * @brief owner確認済み計画状態をRFC4180 escaping済みCSVで返す
 * @param manifestId 呼び出し元のD-Bus接続 (owner) に結び付けられたマニフェストID
 * @return ManifestStatusフィールド順のCSV、失敗時空文字列
 */
QString SnapshotOperations::GetRestorePlanStatus(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return {};
    }

    qsnapper::restore::ManifestError error = qsnapper::restore::ManifestError::None;
    auto status = m_restoreRegistry.status(manifestId, owner, &error);
    if (!status) {
        // 終端してregistryから削除された計画は、保持期間内であればownerにだけ詳細を返す
        status = m_finishedRestorePlans.status(manifestId, owner);
    }
    if (!status) {
        // 期限切れ (Expired) またはregistryに存在しない (NotFound) 計画は再び使われないため、owner表とwatchから外してメモリを解放する
        // OwnerMismatchでは他のownerの計画を消さないよう、ここでは何もしない
        // NotFoundはIDがregistryから消えていることを示すため、終端記録の有無にかかわらず掃除してよい
        if (error == qsnapper::restore::ManifestError::Expired
                || error == qsnapper::restore::ManifestError::NotFound) {
            m_restorePlanOwners.remove(manifestId);
            removeUnusedRestoreOwnerWatches();
        }
        sendManifestError(error);
        return {};
    }

    const QStringList fields{
        status->id,
        restoreManifestStateString(status->state),
        QString::number(status->totalEntries),
        QString::number(status->cursor),
        QString::number(status->processed),
        restoreModeString(status->mode),
        quoteRestoreStatusCsvField(status->configName),
        QString::number(status->snapshotNumber),
        quoteRestoreStatusCsvField(status->lastError),
        QString::number(status->deletedCount),
        QString::number(status->skippedCount),
        QString::number(status->failedIndex),
        quoteRestoreStatusCsvField(status->failedPath),
        // 計画のパスは制御文字を含まないことをステージング時に検証済みのため、改行を区切りに使える
        quoteRestoreStatusCsvField(status->skippedPaths.join(QLatin1Char('\n')))
    };
    return fields.join(QLatin1Char(','));
}

/**
 * @brief owner確認済み非終端計画へ境界cancellationを要求する
 * @param manifestId 呼び出し元のD-Bus接続 (owner) に結び付けられたマニフェストID
 * @return cancellationを受理した場合true
 */
bool SnapshotOperations::CancelRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    if (!m_restoreExecutor.requestCancel(manifestId, owner, &error)) {
        return sendManifestError(error);
    }
    return true;
}
