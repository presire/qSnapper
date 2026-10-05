/**
 * @file snapshotoperations_restore.cpp
 * @brief staged復元計画の開始と確定
 *
 * BeginRestorePlanとStageRestoreEntries、CommitRestorePlan
 * 凍結計画の権威ある比較との照合、ContinueとStatusとCancelを扱う
 * 認可はCommitRestorePlanの中で計画凍結の後に1回だけ行う
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
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * @brief ownerに束縛された空のstaged restore計画を開始する
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元snapshot番号
 * @param counterpartSnapshotNumber 比較相手のsnapshot番号 (0は現在のシステム)
 * @param restoreMode yastまたはdirect
 * @return 成功時マニフェストID
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

    purgeExpiredRestorePlans();

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const QString manifestId = m_restoreRegistry.createStaging(
        owner, ownerUid, *cfg, snapshotNumber, counterpartSnapshotNumber, mode,
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
 * @param manifestId owner束縛されたマニフェストID
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

    // stagingは認可を要さないため、グローバル予算を消費する唯一の経路でもある
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
 * @param manifestId owner束縛されたマニフェストID
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
 *   - 凍結済みの全entryが、サーバが(source, counterpart)から再構築した
 *     権威ある比較と一致すること (クライアント申告のchangeTypeは信頼しない)
 *
 * @param manifestId owner束縛されたマニフェストID
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
    execution.removeOnTypechanged = execution.useReflink;

    try {
        // Snapperは構築時にsnapshot一覧をcaptureし外部追加を観測しないため、
        // 権威ある比較の相手を解決する前に強制再生成する
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
        // 権威ある比較の再構築と全entryの照合、pin済みdirfdの同一性確認が
        // 全て通るまで復元は1バイトもlive filesystemへ書き込まない
        try {
            if (!validateFrozenRestorePlan(manifestId, owner, snapper,
                                           source, counterpart, snapshotDir,
                                           pinnedStat)) {
                failRestorePlanPreflight(manifestId, owner, &error);
                return false;
            }
        }
        catch (const snapper::Exception &) {
            // 例外本文にはlibsnapperが扱ったfilesystem pathが含まれ得るため記録しない
            // (攻撃者由来の名前をログへ持ち込まない)
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (const std::exception &) {
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (...) {
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Staged restore preparation failed:" << e.what();
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                        QStringLiteral("Failed to prepare restore plan"));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << "Staged restore preparation failed unexpectedly:"
                   << e.what();
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

/**
 * @brief 凍結済み計画をサーバ再構築の権威ある比較と照合して検証する
 * @param manifestId owner束縛されたマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @param snapper 検証済み設定のSnapperインスタンス
 * @param source 復元元snapshotのiterator
 * @param counterpart 比較相手snapshotのiterator
 * @param snapshotDir pin済みdirfdが指すsnapshot directoryのpath
 * @param pinnedStat pin済みdirfdに対するfstatの結果
 * @return 計画全体が権威ある比較と一致し同一性確認も通った場合true
 *
 * @note 本検証はbest-effortの整合性検査であり、tamper-proofなセキュリティ境界ではない
 *       snapper::Comparisonはread-only snapshot同士ではlibsnapper保存のfilelistキャッシュを
 *       優先して読み、そのキャッシュは無エスケープの行指向形式であるため、
 *       改行を含むファイル名により偽entryが混入し得る (詳細はrestorevalidation.hの注記)
 *       キャッシュを公開APIから無効化できないため本経路は塞げない
 *       実際の防護は実行段のisConfirmedAbsentAt()とsource種別判定が担い、
 *       こちらは認可時にpinしたdirfdから直接算出するためキャッシュの影響を受けない
 */
bool SnapshotOperations::validateFrozenRestorePlan(
    const QString &manifestId,
    const QString &owner,
    snapper::Snapper *snapper,
    const snapper::Snapshots::const_iterator &source,
    const snapper::Snapshots::const_iterator &counterpart,
    const QString &snapshotDir,
    const struct stat &pinnedStat)
{
    QHash<QString, QString> authoritative;

    {
        // ローカルの非キャッシュComparison (mount=false)
        // mount=falseでもinitialize()はstatus setを計算するため、
        // getName() / getPreToPostStatus()は使用可能である
        // blockを抜けるまでにComparisonを使い切り、cleanupやexecutor起動の
        // 前に確実に破棄する
        snapper::Comparison comparison(snapper, source, counterpart, false);

        const snapper::Files &files = comparison.getFiles();
        authoritative.reserve(static_cast<qsizetype>(files.size()));
        for (const snapper::File &file : files) {
            // 権威名は復号可能性と制御文字の両方を検証する
            // libsnapperのfilelistキャッシュは無エスケープの行指向形式であり、
            // 改行を含む名前はキャッシュ再読込時に偽entryとして混入し得る
            // 攻撃者が制御する名前はログにもエラー本文にも載せない
            QString authoritativeName;
            if (!qsnapper::restore::decodeAuthoritativeRestoreName(
                    file.getName(), &authoritativeName)) {
                qWarning() << "Staged restore: Authoritative comparison contains"
                           << "an unusable path name";
                return false;
            }

            if (!qsnapper::restore::registerAuthoritativeRestoreEntry(
                    &authoritative, authoritativeName,
                    file.getPreToPostStatus())) {
                // 同一パスに異なる期待型が潰れて衝突した場合は検証不能であり、
                // 計画全体をfail-closedさせる
                qWarning() << "Staged restore: Authoritative entries contain"
                           << "conflicting change types for one path";
                return false;
            }
        }

        qsnapper::restore::ManifestError validationError =
            qsnapper::restore::ManifestError::None;
        if (!qsnapper::restore::validateFrozenEntriesAgainstAuthoritative(
                m_restoreRegistry, manifestId, owner, authoritative,
                &validationError)) {
            qWarning() << "Staged restore: Frozen plan does not match the"
                       << "authoritative comparison";
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
 * @param manifestId owner束縛されたマニフェストID
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
 * @param manifestId owner束縛されたマニフェストID
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
        quoteRestoreStatusCsvField(status->lastError)
    };
    return fields.join(QLatin1Char(','));
}

/**
 * @brief owner確認済み非終端計画へ境界cancellationを要求する
 * @param manifestId owner束縛されたマニフェストID
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
