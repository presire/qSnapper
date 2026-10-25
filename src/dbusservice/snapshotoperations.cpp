/**
 * @file snapshotoperations.cpp
 * @brief SnapshotOperationsの生成破棄と実行時コンテキスト管理
 *
 * コンストラクタとデストラクタ、アイドルタイマ、応答文言の整形
 * 復元実行コンテキストの後片付けとrootサブボリューム安全ネットを扱う
 * 認可ゲートやスナップショット操作本体は他ファイルに分離している
 */

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <btrfsutil.h>
#include <snapper/Snapper.h>
#include <snapper/Comparison.h>
#include <snapper/Exception.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <string>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "csvrecord.h"
#include "filesystemhelpers.h"

// pin済み復元元の読み取り専用判定と認可待ち予算の実装は本ファイルに置く
bool qsnapper::restore::isPinnedRestoreSourceReadOnly(int snapshotDirFd, const std::string &fstype)
{
    if (fstype == "btrfs") {
        bool readOnly = false;
        return btrfs_util_get_subvolume_read_only_fd(snapshotDirFd, &readOnly) == BTRFS_UTIL_OK
               && readOnly;
    }

    struct statvfs fsInfo = {};
    return ::fstatvfs(snapshotDirFd, &fsInfo) == 0 && (fsInfo.f_flag & ST_RDONLY) != 0;
}

/**
 * @brief 上限を指定して構築する
 * @param perUidLimit 1つのUIDが同時に保持できる件数の上限
 * @param globalLimit 全体で同時に保持できる件数の上限
 */
qsnapper::security::PendingAuthorizationBudget::PendingAuthorizationBudget(int perUidLimit, int globalLimit)
    : m_perUidLimit(perUidLimit)
    , m_globalLimit(globalLimit)
{
}

/**
 * @brief 1件分の枠を確保する
 * @param uid 呼び出し元のUID
 * @return UID単位と全体の両方の上限内であれば確保してtrue、それ以外は何もせずfalse
 */
bool qsnapper::security::PendingAuthorizationBudget::tryAcquire(uint uid)
{
    if (m_total >= m_globalLimit || m_perUid.value(uid, 0) >= m_perUidLimit) {
        return false;
    }
    ++m_perUid[uid];
    ++m_total;
    return true;
}

/**
 * @brief tryAcquire()で確保した1件分の枠を返す
 * @param uid tryAcquire()に渡したUID
 */
void qsnapper::security::PendingAuthorizationBudget::release(uint uid)
{
    const auto it = m_perUid.find(uid);
    if (it == m_perUid.end()) {
        return;
    }
    if (--it.value() <= 0) {
        m_perUid.erase(it);
    }
    if (m_total > 0) {
        --m_total;
    }
}

/**
 * @brief 全体の保持件数を返す
 * @return 保持件数
 */
int qsnapper::security::PendingAuthorizationBudget::total() const
{
    return m_total;
}

/**
 * @brief 指定したUIDの保持件数を返す
 * @param uid 呼び出し元のUID
 * @return 保持件数
 */
int qsnapper::security::PendingAuthorizationBudget::countFor(uint uid) const
{
    return m_perUid.value(uid, 0);
}

/**
 * @brief SnapshotOperationsクラスのコンストラクタ
 *
 * スナップショット操作を管理するクラスを初期化する
 *
 * @param parent 親QObjectポインタ
 */
SnapshotOperations::SnapshotOperations(QObject *parent)
    : QObject(parent)
    , m_snapper(nullptr)
    , m_currentConfig("")
    , m_restoreExecutor(m_restoreRegistry)
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(IdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, []() {
        qInfo() << "Idle timeout reached, shutting down...";
        QCoreApplication::quit();
    });
    m_idleTimer.start();

    m_restoreExecutor.setEntryApplier(
        [this](const QString &manifestId,
               const qsnapper::restore::RestoreEntry &entry) {
            return applyRestoreEntry(manifestId, entry);
        });
    m_restoreExecutor.setProgressSink(
        [this](const QString &manifestId, int current, int total,
               const QString &path) {
            Q_UNUSED(path);
            resetIdleTimer();
            const QString owner = m_restorePlanOwners.value(manifestId);
            if (sendRestorePlanSignalToOwner(owner, QStringLiteral("restorePlanProgress"),
                                             QVariantList{manifestId, current, total})) {
                return;
            }
            emit restorePlanProgress(manifestId, current, total);
        });
    m_restoreExecutor.setFinishedSink(
        [this](const QString &manifestId,
               qsnapper::restore::ManifestState terminal,
               const QString &message) {
            finishRestorePlan(manifestId, terminal, message);
        });
    m_restoreExecutor.setChunkScheduler(
        [this](std::function<void()> chunk) {
            QTimer::singleShot(0, this, [chunk]() { chunk(); });
        });

    m_rootReadOnlyProbe = [](bool *readOnly) {
        return btrfs_util_get_subvolume_read_only("/", readOnly) == BTRFS_UTIL_OK;
    };
    m_rootReadWriteRestorer = []() {
        const auto result = btrfs_util_set_subvolume_read_only("/", false);
        if (result != BTRFS_UTIL_OK) {
            qCritical() << "Staged restore: Failed to restore root subvolume rw state:"
                        << result;
            return false;
        }
        return true;
    };

    m_ownerWatcher = new QDBusServiceWatcher(
        QString(), QDBusConnection::systemBus(),
        QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_ownerWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &SnapshotOperations::handleRestoreOwnerUnregistered);
}

/**
 * @brief SnapshotOperationsクラスのデストラクタ
 *
 * リソースのクリーンアップを行う
 */
SnapshotOperations::~SnapshotOperations()
{
    const QStringList planIds = m_restorePlanOwners.keys();
    for (const QString &manifestId : planIds) {
        m_restoreExecutor.abandon(manifestId);
    }

    const QStringList executionIds = m_restoreExecutions.keys();
    for (const QString &manifestId : executionIds) {
        cleanupRestoreExecution(manifestId);
    }
}

/**
 * @brief アイドルタイマをリセット
 *
 * D-Busメソッド呼び出し時にタイマをリセットし、アイドルタイムアウトを延長する
 *
 * polkitプロンプトの応答待ちが1件でも残っている間はタイマを止めたままにする
 * (プロンプトはタイムアウトを持たないため、認証中にアイドル終了してしまうと、ユーザがパスワードを入力した直後に呼び出しが失われる)
 */
void SnapshotOperations::resetIdleTimer()
{
    // timerの操作 (変更と照会) の途中で終了すると、systemdへの変更が途中で止まったり、呼び出し元へ結果を返せなくなるため、完了まで終了しない
    // snapperdへ転送した変更も同じ理由で、完了 (結果が分からない場合はsnapperdの終了) まで終了しない
    if (m_pendingAuthorizations.total() > 0 || m_timerOperation || m_timerQueryOperation || m_snapperdOperation) {
        m_idleTimer.stop();
        return;
    }
    m_idleTimer.start();
}

/**
 * @brief 現在のD-Bus呼び出し元unique nameを返す
 * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
 */
QString SnapshotOperations::callerOwner() const
{
    return calledFromDBus() ? message().service() : QString();
}

/**
 * @brief manifest操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
 * @param error registryが返したエラー
 * @return 常にfalse
 */
bool SnapshotOperations::sendManifestError(
    qsnapper::restore::ManifestError error)
{
    using qsnapper::restore::ManifestError;

    QDBusError::ErrorType errorType = QDBusError::Failed;
    QString messageText = QStringLiteral("Restore plan operation failed");

    switch (error) {
    case ManifestError::NotFound:
    case ManifestError::OwnerMismatch:
    case ManifestError::Expired:
        errorType = QDBusError::AccessDenied;
        messageText = QStringLiteral("Restore plan access denied");
        break;
    case ManifestError::WrongState:
        errorType = QDBusError::Failed;
        messageText = QStringLiteral("Restore plan is not in the required state");
        break;
    case ManifestError::AlreadyTerminal:
        errorType = QDBusError::Failed;
        messageText = QStringLiteral("Restore plan is already terminal");
        break;
    case ManifestError::CapacityExceeded:
        errorType = QDBusError::InvalidArgs;
        messageText = QStringLiteral("Restore plan capacity exceeded");
        break;
    case ManifestError::GlobalLimit:
        errorType = QDBusError::LimitsExceeded;
        messageText = QStringLiteral("Restore plan limit exceeded");
        break;
    case ManifestError::InvalidArgument:
        errorType = QDBusError::InvalidArgs;
        messageText = QStringLiteral("Invalid restore plan request");
        break;
    case ManifestError::None:
        break;
    }

    replyError(errorType, messageText);
    return false;
}

/**
 * @brief マニフェスト状態をD-Bus contractの小文字表現へ変換する
 * @param state 変換対象状態
 * @return contractで定義した状態文字列
 */
QString SnapshotOperations::restoreManifestStateString(
    qsnapper::restore::ManifestState state)
{
    using qsnapper::restore::ManifestState;

    switch (state) {
    case ManifestState::Staging:
        return QStringLiteral("staging");
    case ManifestState::Frozen:
        return QStringLiteral("frozen");
    case ManifestState::Running:
        return QStringLiteral("running");
    case ManifestState::Completed:
        return QStringLiteral("completed");
    case ManifestState::Failed:
        return QStringLiteral("failed");
    case ManifestState::Cancelled:
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("failed");
}

/**
 * @brief restorePlanFinished signalに載せる終端状態ごとの固定文言を返す
 * @param state 終端状態
 * @return pathなどの詳細を含まない固定文言
 */
QString SnapshotOperations::restorePlanFinishedSignalMessage(
    qsnapper::restore::ManifestState state)
{
    using qsnapper::restore::ManifestState;

    switch (state) {
    case ManifestState::Completed:
        return QStringLiteral("Restore completed");
    case ManifestState::Cancelled:
        return QStringLiteral("Restore cancelled");
    case ManifestState::Staging:
    case ManifestState::Frozen:
    case ManifestState::Running:
    case ManifestState::Failed:
        break;
    }
    return QStringLiteral("Restore failed");
}

/**
 * @brief 復元方式をD-Bus contractの文字列表現へ変換する
 * @param mode 変換対象方式
 * @return yastまたはdirect
 */
QString SnapshotOperations::restoreModeString(
    qsnapper::restore::RestoreMode mode)
{
    return mode == qsnapper::restore::RestoreMode::DirectCopy
        ? QStringLiteral("direct")
        : QStringLiteral("yast");
}

/**
 * @brief RFC4180形式で必要なCSVフィールドをクォートする
 * @param フィールドクォート対象文字列
 * @return CSVへ安全に埋め込めるフィールド
 *
 * 実装はqsnapper::csv::quoteField() (include/csvrecord.h) へ委譲する
 * スナップショット一覧CSVと同じ実装を使うことでproducer/consumerの対称性を保証する
 * 共有実装はCR / LFもクォート対象とする (旧実装の上位互換)
 *
 * クォート対象のconfigName / lastErrorは、それぞれ設定名検証とqSnapper内部生成のメッセージに由来し実際にはCR / LFを含まないため、出力は従来と同一である
 */
QString SnapshotOperations::quoteRestoreStatusCsvField(const QString &field)
{
    return qsnapper::csv::quoteField(field);
}

/**
 * @brief execution contextに記録されたsnapshot mountを解除する
 * @param execution mount元設定とsnapshot番号を持つcontext
 */
void SnapshotOperations::unmountRestoreExecution(
    const RestoreExecution &execution)
{
    if (!execution.mounted) {
        return;
    }

    try {
        snapper::Snapper *snapper = getSnapper(execution.configName);
        if (!snapper) {
            qWarning() << "Staged restore: Failed to initialize Snapper for unmount";
            return;
        }

        const snapper::Snapshots::const_iterator snapshot =
            snapper->getSnapshots().find(execution.snapshotNumber);
        if (snapshot == snapper->getSnapshots().end()) {
            qWarning() << "Staged restore: Snapshot unavailable during unmount";
            return;
        }
        snapshot->umountFilesystemSnapshot(true);
    }
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Staged restore: Failed to unmount snapshot";
    }
    catch (const std::exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Staged restore: Unexpected unmount failure";
    }
    catch (...) {
        qWarning() << "Staged restore: Unknown unmount failure";
    }
}

/**
 * @brief 指定マニフェストのmountを解除して実行contextを削除する
 * @param manifestId cleanup対象マニフェストID
 */
void SnapshotOperations::cleanupRestoreExecution(const QString &manifestId)
{
    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution == m_restoreExecutions.end()) {
        return;
    }

    // dirfdを先に閉じてからunmountを要求する
    // 開いたfdがmount点をbusyのままにすると、検証失敗時の後片付けが
    // mount leakに化けるため、順序を逆にしてはならない
    if (execution->snapshotDirFd >= 0) {
        ::close(execution->snapshotDirFd);
        execution->snapshotDirFd = -1;
    }
    const RestoreExecution context = execution.value();
    unmountRestoreExecution(context);
    m_restoreExecutions.erase(execution);
}

/**
 * @brief rootサブボリュームのread-only状態の取得とrw化の処理をテスト用に差し替える
 * @param readOnlyProbe read-only状態を取得する関数 (取得できた場合true)
 * @param readWriteRestorer rwへ戻す関数 (成功した場合true)
 */
void SnapshotOperations::setRootSubvolumeAccessForTesting(
    std::function<bool(bool *)> readOnlyProbe,
    std::function<bool()> readWriteRestorer)
{
    m_rootReadOnlyProbe = std::move(readOnlyProbe);
    m_rootReadWriteRestorer = std::move(readWriteRestorer);
}

/**
 * @brief 実行を開始した復元計画に限り、rootサブボリュームをrwへ戻す安全ネットを実行する
 * @param state commit時に記録した安全ネットの判定材料
 * @return 安全ネットが不要だった場合、またはrwであることを確認・回復できた場合true
 *         状態を取得できない場合とrwへ戻せなかった場合はfalse (rootがread-onlyのまま残っている可能性がある)
 */
bool SnapshotOperations::runRootReadWriteSafetyNet(
    const qsnapper::restore::RootReadWriteSafetyNetState &state)
{
    // 復元後にread-onlyだったと仮定しても条件を満たさない計画では、状態の取得すら行わない
    // 復元の実行を認可されていないBegin -> Cancelなどから特権操作へ到達させないため
    if (!qsnapper::restore::shouldRestoreRootReadWrite(state, true)) {
        return true;
    }

    bool readOnly = false;
    if (!m_rootReadOnlyProbe || !m_rootReadOnlyProbe(&readOnly)) {
        qWarning() << "Staged restore: Failed to read root subvolume read-only state";
        return false;
    }
    if (!qsnapper::restore::shouldRestoreRootReadWrite(state, readOnly)) {
        return true;
    }

    qWarning() << "Staged restore: Root subvolume became read-only after restore, restoring rw";
    if (!m_rootReadWriteRestorer || !m_rootReadWriteRestorer()) {
        qCritical() << "Staged restore: Failed to make the root subvolume writable again";
        return false;
    }
    return true;
}

/**
 * @brief 実行を開始した計画の後片付けを決まった順序で行い、実行contextを削除する
 *
 * 安全ネット → ディレクトリの時刻 → dirfdを閉じる → unmountの順に行う
 * 正常な終端とowner消失のどちらでも同じ順序にし、rootサブボリュームのrw化や時刻の復元が経路によって抜けないようにする
 * 実行contextは認可済みcommitでのみ作られるため、復元の実行を認可される前の計画では何もしない
 *
 * @param manifestId 対象のマニフェストID
 * @return rootサブボリュームがread-onlyのまま残った可能性がある場合true
 */
bool SnapshotOperations::finalizeRestoreExecution(const QString &manifestId)
{
    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution == m_restoreExecutions.end()) {
        return false;
    }

    const bool rootLeftReadOnly = !runRootReadWriteSafetyNet(execution->rootReadWriteState);

    // 子entryの適用で更新されたディレクトリの時刻を、全entryの適用後にまとめて復元元に合わせる
    // 失敗・中止で終わった場合も、適用済みのディレクトリは復元元の状態にそろえる
    applyPendingDirectoryTimes(execution->pendingDirectoryTimes);

    // cleanupRestoreExecutionと同じ順序規約: dirfdを先に閉じる
    if (execution->snapshotDirFd >= 0) {
        ::close(execution->snapshotDirFd);
        execution->snapshotDirFd = -1;
    }
    unmountRestoreExecution(execution.value());
    m_restoreExecutions.erase(execution);
    return rootLeftReadOnly;
}

/**
 * @brief 復元計画のシグナルを所有者のunique nameだけへ送る
 *
 * 進捗と終了はシステムバス上の全利用者へブロードキャストせず、計画を作成した接続だけに届ける
 * 所有者が空の場合はD-Busを介さない単体テスト呼び出しなので、QSignalSpyで観測できるよう呼び出し側のemitに任せる
 *
 * @param owner 宛先のD-Bus unique name
 * @param member シグナル名
 * @param arguments シグナルの引数
 * @return true = 宛先付きで送信を試みた (呼び出し側はemitしない)、false = ownerが空のため呼び出し側でemitする
 */
bool SnapshotOperations::sendRestorePlanSignalToOwner(const QString &owner,
                                                      const QString &member,
                                                      const QVariantList &arguments)
{
    if (owner.isEmpty()) {
        return false;
    }

    QDBusMessage signal = QDBusMessage::createTargetedSignal(
        owner,
        QStringLiteral("/com/presire/qsnapper/Operations"),
        QStringLiteral("com.presire.qsnapper.Operations"),
        member);
    signal.setArguments(arguments);
    if (!QDBusConnection::systemBus().send(signal)) {
        // 送信に失敗しても、全体へのブロードキャストには切り替えない
        // 所有者へ通知が届かないことより、他の利用者に復元の進行状況が漏れることの方を避ける
        qWarning() << "Failed to send restore plan signal" << member << "to" << owner;
    }
    return true;
}

/**
 * @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
 * @param manifestId 終端したマニフェストID
 * @param terminal 終端状態
 * @param messageText 終端理由
 */
void SnapshotOperations::finishRestorePlan(
    const QString &manifestId,
    qsnapper::restore::ManifestState terminal,
    const QString &messageText)
{
    const bool rootLeftReadOnly = finalizeRestoreExecution(manifestId);

    // 詳細 (失敗したpathを含み得る) はownerだけが取得できるように残し、signalには固定文言だけを載せる
    const QString owner = m_restorePlanOwners.value(manifestId);
    qsnapper::restore::ManifestStatus finishedStatus =
        m_restoreRegistry.status(manifestId, owner, nullptr)
            .value_or(qsnapper::restore::ManifestStatus{});
    // rootサブボリュームがread-onlyのまま残った可能性がある場合は、entryを全て適用できていても成功として報告しない
    // 管理者が気付かないまま再起動するとシステムが書き込み不能になるため、Failedと理由を必ずownerへ返す
    if (rootLeftReadOnly) {
        terminal = qsnapper::restore::ManifestState::Failed;
        const QString rootMessage =
            QStringLiteral("The root subvolume could not be confirmed writable after restore");
        finishedStatus.lastError = finishedStatus.lastError.isEmpty()
                ? rootMessage
                : finishedStatus.lastError + QStringLiteral("; ") + rootMessage;
    }
    finishedStatus.id = manifestId;
    finishedStatus.state = terminal;
    if (finishedStatus.lastError.isEmpty()) {
        finishedStatus.lastError = messageText;
    }
    m_finishedRestorePlans.record(owner, finishedStatus);

    const QString terminalState = restoreManifestStateString(terminal);
    const QString signalMessage = restorePlanFinishedSignalMessage(terminal);
    if (!sendRestorePlanSignalToOwner(owner, QStringLiteral("restorePlanFinished"),
                                      QVariantList{manifestId, terminalState, signalMessage})) {
        emit restorePlanFinished(manifestId, terminalState, signalMessage);
    }

    m_restoreExecutions.remove(manifestId);
    m_restoreRegistry.remove(manifestId);
    m_restorePlanOwners.remove(manifestId);
    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief owner消失時に予約済み実行とmountとマニフェストを全て破棄する
 * @param owner unregisterされたD-Bus unique name
 */
void SnapshotOperations::handleRestoreOwnerUnregistered(const QString &owner)
{
    QStringList ownedPlanIds;
    for (auto plan = m_restorePlanOwners.cbegin();
         plan != m_restorePlanOwners.cend(); ++plan) {
        if (plan.value() == owner) {
            ownedPlanIds.append(plan.key());
        }
    }

    for (const QString &manifestId : ownedPlanIds) {
        m_restoreExecutor.abandon(manifestId);
        // 実行中にGUIが終了しても、rootサブボリュームのrw化とディレクトリの時刻の復元を正常な終端と同じ順序で行う
        // 結果を受け取るownerはもういないため、rw化の失敗はrunRootReadWriteSafetyNetのログだけで知らせる
        finalizeRestoreExecution(manifestId);
        m_restorePlanOwners.remove(manifestId);
    }

    m_restoreRegistry.removeByOwner(owner);
    m_finishedRestorePlans.removeByOwner(owner);
    if (m_ownerWatcher && m_restoreRegistry.countForOwner(owner) == 0) {
        m_ownerWatcher->removeWatchedService(owner);
    }
}

/**
 * @brief TTL purgeで消えたactive計画をabandonしmountもcleanupする
 */
void SnapshotOperations::purgeExpiredRestorePlans()
{
    m_restoreRegistry.purgeExpired();
    m_finishedRestorePlans.purgeExpired();

    const QStringList planIds = m_restorePlanOwners.keys();
    for (const QString &manifestId : planIds) {
        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const QString owner = m_restorePlanOwners.value(manifestId);
        if (!m_restoreRegistry.status(manifestId, owner, &error)) {
            // executorが同じ計画をもう1度終端しないよう先にabandonしてから、finishRestorePlanで終端する
            // ここでcleanupRestoreExecutionだけを呼ぶとrestorePlanFinishedが発火せず、
            // クライアントは完了通知を永久に待ち続け、復元でread-onlyになったrootサブボリュームをrwへ戻す安全ネットも実行されない
            m_restoreExecutor.abandon(manifestId);
            finishRestorePlan(
                manifestId, qsnapper::restore::ManifestState::Failed,
                QStringLiteral("Restore plan expired before completion"));
        }
    }

    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief マニフェストを持たないownerをservice watcherから除外する
 */
void SnapshotOperations::removeUnusedRestoreOwnerWatches()
{
    if (!m_ownerWatcher) {
        return;
    }

    const QStringList watchedOwners = m_ownerWatcher->watchedServices();
    for (const QString &owner : watchedOwners) {
        // 終端した計画も保持期間中はownerの切断時に掃除する必要があるため、watchを残す
        if (m_restoreRegistry.countForOwner(owner) == 0
                && m_finishedRestorePlans.countForOwner(owner) == 0) {
            m_ownerWatcher->removeWatchedService(owner);
        }
    }
}
