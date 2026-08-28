#include "restoreplanexecutor.h"

#include <QQueue>
#include <QTest>
#include <QVector>

#include <functional>
#include <utility>

using namespace qsnapper::restore;

namespace {

constexpr qint64 kInitialTimeMs = 1000;
const QString kOwner = QStringLiteral(":1.1");
const QString kOtherOwner = QStringLiteral(":1.2");

/**
 * @brief ManifestErrorをQCOMPARE可能な整数へ変換する
 * @param error 変換するエラー
 * @return 列挙値に対応する整数
 */
int errorValue(ManifestError error)
{
    return static_cast<int>(error);
}

/**
 * @brief callbackの呼び出し結果を保持するrecording sink
 */
struct CallbackRecord {
    QStringList appliedPaths;
    QVector<int> progressCurrent;
    QVector<int> progressTotal;
    QStringList progressPaths;
    QVector<ManifestState> finishedStates;
    QStringList finishedMessages;
    QString failingPath;
};

/**
 * @brief chunkを明示的に一つずつ実行できるmanual scheduler
 */
class ManualScheduler
{
public:
    /**
     * @brief chunkを実行せずqueueへ積む
     * @param chunk 予約する処理
     */
    void enqueue(std::function<void()> chunk)
    {
        ++scheduleCount;
        m_chunks.enqueue(std::move(chunk));
    }

    /**
     * @brief queue先頭のchunkを一つ実行する
     * @return 実行対象が存在した場合true
     */
    bool drainOne()
    {
        if (m_chunks.isEmpty()) {
            return false;
        }
        std::function<void()> chunk = m_chunks.dequeue();
        chunk();
        return true;
    }

    /**
     * @brief 新たに予約されるchunkを含めqueueが空になるまで実行する
     */
    void drainAll()
    {
        while (drainOne()) {
        }
    }

    /**
     * @brief 現在queueに残るchunk数を返す
     * @return 未実行chunk数
     */
    int queuedCount() const
    {
        return m_chunks.size();
    }

    int scheduleCount = 0;

private:
    QQueue<std::function<void()>> m_chunks;
};

/**
 * @brief 指定件数の絶対path列を生成する
 * @param count 生成件数
 * @return /entry-N形式のpath列
 */
QStringList makePaths(int count)
{
    QStringList paths;
    paths.reserve(count);
    for (int index = 0; index < count; ++index) {
        paths.append(QStringLiteral("/entry-%1").arg(index));
    }
    return paths;
}

/**
 * @brief path列と同数のmodified列を生成する
 * @param count 生成件数
 * @return change type列
 */
QStringList makeChangeTypes(int count)
{
    return QStringList(count, QStringLiteral("modified"));
}

/**
 * @brief 非空entry列を持つFrozen計画を作成する
 * @param registry 使用するregistry
 * @param paths stageするpath列
 * @param changeTypes stageするchange type列
 * @param owner 計画owner
 * @return 成功時manifest id、失敗時空文字列
 */
QString createFrozenPlan(RestoreManifestRegistry &registry,
                         const QStringList &paths,
                         const QStringList &changeTypes,
                         const QString &owner = kOwner)
{
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        owner, QStringLiteral("root"), 42,
        RestoreMode::YastCompatible, &error);
    if (id.isEmpty()
            || !registry.stageEntries(id, owner, paths, changeTypes, &error)
            || !registry.freeze(id, owner, &error)) {
        return {};
    }
    return id;
}

/**
 * @brief executorへmanual schedulerとrecording callbacksを設定する
 * @param executor 設定対象executor
 * @param scheduler chunkを保持するmanual scheduler
 * @param record callback記録先
 */
void configureExecutor(RestorePlanExecutor &executor,
                       ManualScheduler &scheduler,
                       CallbackRecord &record)
{
    executor.setChunkScheduler(
        [&scheduler](std::function<void()> chunk) {
            scheduler.enqueue(std::move(chunk));
        });
    executor.setEntryApplier(
        [&record](const QString &id, const RestoreEntry &entry) {
            Q_UNUSED(id)
            record.appliedPaths.append(entry.path);
            return entry.path != record.failingPath;
        });
    executor.setProgressSink(
        [&record](const QString &id, int current, int total,
                  const QString &path) {
            Q_UNUSED(id)
            record.progressCurrent.append(current);
            record.progressTotal.append(total);
            record.progressPaths.append(path);
        });
    executor.setFinishedSink(
        [&record](const QString &id, ManifestState terminal,
                  const QString &message) {
            Q_UNUSED(id)
            record.finishedStates.append(terminal);
            record.finishedMessages.append(message);
        });
}

} // namespace

/**
 * @brief RestorePlanExecutorのowner境界とchunk状態機械を検証する
 */
class TestRestorePlanExecutor : public QObject
{
    Q_OBJECT

private slots:
    void happyPathCompletesInOrder();
    void multiChunkProgressUsesOriginalTotal();
    void crossOwnerCallsDoNotInvokeCallbacks();
    void completedPlanRejectsReplay();
    void expiredPlanDoesNotInvokeCallbacks();
    void abandonedPlanDropsQueuedChunks();
    void cancelAtChunkBoundaryStopsLaterEntries();
    void applierFailureFailsOnce();
    void continueOnFrozenPlanIsRejectedAsWrongState();
    void cancelTwiceIsRejectedAsAlreadyTerminal();
    void continueAfterCancelDoesNotResumeWork();
    void planStalledPastTtlDuringExecutionKeepsRunning();
    void vanishedPlanDuringExecutionEmitsFailedTerminal();
    void longRunningEntriesKeepPlanAlive();
};

/**
 * @brief 全entryを順序通り適用してCompletedを一度通知することを検証する
 */
void TestRestorePlanExecutor::happyPathCompletesInOrder()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const QStringList paths{
        QStringLiteral("/one"),
        QStringLiteral("/two"),
        QStringLiteral("/three")
    };
    const QString id = createFrozenPlan(registry, paths,
                                        makeChangeTypes(paths.size()));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;

    QVERIFY(executor.start(id, kOwner, &error));
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(scheduler.queuedCount(), 1);
    scheduler.drainAll();

    QCOMPARE(record.appliedPaths, paths);
    const auto status = registry.status(id, kOwner, &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->state, ManifestState::Completed);
    QCOMPARE(status->cursor, paths.size());
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Completed);
}

/**
 * @brief 複数chunkのschedule回数と元のtotalに対する単調progressを検証する
 */
void TestRestorePlanExecutor::multiChunkProgressUsesOriginalTotal()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int total = RestorePlanExecutor::kEntriesPerExecutionChunk * 2 + 5;
    const QStringList paths = makePaths(total);
    const QString id = createFrozenPlan(registry, paths,
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    scheduler.drainAll();

    QVERIFY(scheduler.scheduleCount > 1);
    QCOMPARE(record.appliedPaths, paths);
    QCOMPARE(record.progressCurrent.size(), total);
    QCOMPARE(record.progressTotal.size(), total);
    for (int index = 0; index < total; ++index) {
        QCOMPARE(record.progressCurrent.at(index), index + 1);
        QCOMPARE(record.progressTotal.at(index), total);
    }
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Completed);
}

/**
 * @brief 異なるownerのstart・continue・cancelがcallbackを一切発火しないことを検証する
 */
void TestRestorePlanExecutor::crossOwnerCallsDoNotInvokeCallbacks()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const QString id = createFrozenPlan(
        registry, {QStringLiteral("/one")},
        {QStringLiteral("modified")});
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;

    QVERIFY(!executor.start(id, kOtherOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.progressCurrent.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);

    QVERIFY(executor.start(id, kOwner, &error));
    QVERIFY(!executor.requestContinue(id, kOtherOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    QVERIFY(!executor.requestCancel(id, kOtherOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.progressCurrent.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);

    executor.abandon(id);
    registry.removeByOwner(kOwner);
    scheduler.drainAll();
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);
}

/**
 * @brief Completed計画のstart・continue replayがcallbackを増やさないことを検証する
 */
void TestRestorePlanExecutor::completedPlanRejectsReplay()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const QString id = createFrozenPlan(
        registry, {QStringLiteral("/one")},
        {QStringLiteral("modified")});
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));
    scheduler.drainAll();
    const int appliedBeforeReplay = record.appliedPaths.size();
    const int progressBeforeReplay = record.progressCurrent.size();
    const int finishedBeforeReplay = record.finishedStates.size();

    QVERIFY(!executor.start(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));
    QVERIFY(!executor.requestContinue(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));
    QCOMPARE(record.appliedPaths.size(), appliedBeforeReplay);
    QCOMPARE(record.progressCurrent.size(), progressBeforeReplay);
    QCOMPARE(record.finishedStates.size(), finishedBeforeReplay);
}

/**
 * @brief TTL超過後のstartがExpiredとなりcallbackを発火しないことを検証する
 */
void TestRestorePlanExecutor::expiredPlanDoesNotInvokeCallbacks()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const QString id = createFrozenPlan(
        registry, {QStringLiteral("/one")},
        {QStringLiteral("modified")});
    QVERIFY(!id.isEmpty());
    nowMs += RestoreManifestRegistry::kDefaultTtlMs + 1;
    ManifestError error = ManifestError::None;

    QVERIFY(!executor.start(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::Expired));
    QCOMPARE(scheduler.scheduleCount, 0);
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.progressCurrent.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);
}

/**
 * @brief owner消失相当のabandon後は予約済みchunkが何も発火しないことを検証する
 */
void TestRestorePlanExecutor::abandonedPlanDropsQueuedChunks()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int total = RestorePlanExecutor::kEntriesPerExecutionChunk + 1;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    executor.abandon(id);
    registry.removeByOwner(kOwner);
    scheduler.drainAll();

    QVERIFY(!executor.isActive(id));
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.progressCurrent.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);
}

/**
 * @brief 最初のchunk後のcancelが後続entryを止めCancelledを一度通知することを検証する
 */
void TestRestorePlanExecutor::cancelAtChunkBoundaryStopsLaterEntries()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int firstChunk = RestorePlanExecutor::kEntriesPerExecutionChunk;
    const int total = firstChunk + 7;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));
    QVERIFY(scheduler.drainOne());
    QCOMPARE(record.appliedPaths.size(), firstChunk);
    QCOMPARE(record.progressCurrent.size(), firstChunk);
    QVERIFY(scheduler.queuedCount() > 0);

    QVERIFY(executor.requestCancel(id, kOwner, &error));
    const auto cancelled = registry.status(id, kOwner, &error);
    QVERIFY(cancelled.has_value());
    QCOMPARE(cancelled->state, ManifestState::Cancelled);
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Cancelled);

    scheduler.drainAll();
    QCOMPARE(record.appliedPaths.size(), firstChunk);
    QCOMPARE(record.progressCurrent.size(), firstChunk);
    QCOMPARE(record.finishedStates.size(), 1);
}

/**
 * @brief applier失敗が後続entryを止めFailedを一度通知することを検証する
 */
void TestRestorePlanExecutor::applierFailureFailsOnce()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    record.failingPath = QStringLiteral("/two");
    configureExecutor(executor, scheduler, record);

    const QStringList paths{
        QStringLiteral("/one"),
        QStringLiteral("/two"),
        QStringLiteral("/three")
    };
    const QString id = createFrozenPlan(registry, paths,
                                        makeChangeTypes(paths.size()));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    scheduler.drainAll();

    QCOMPARE(record.appliedPaths,
             QStringList({QStringLiteral("/one"), QStringLiteral("/two")}));
    QCOMPARE(record.progressCurrent, QVector<int>({1}));
    const auto failed = registry.status(id, kOwner, &error);
    QVERIFY(failed.has_value());
    QCOMPARE(failed->state, ManifestState::Failed);
    QCOMPARE(failed->cursor, 1);
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Failed);
    QCOMPARE(record.finishedMessages.size(), 1);
    QVERIFY(record.finishedMessages.first().contains(QStringLiteral("/two")));
}

/**
 * @brief startされていないFrozen計画へのrequestContinueがWrongStateで拒否されることを検証する
 */
void TestRestorePlanExecutor::continueOnFrozenPlanIsRejectedAsWrongState()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const QString id = createFrozenPlan(
        registry, {QStringLiteral("/one")},
        {QStringLiteral("modified")});
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;

    QVERIFY(!executor.requestContinue(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::WrongState));
    QCOMPARE(record.appliedPaths.size(), 0);
    QCOMPARE(record.progressCurrent.size(), 0);
    QCOMPARE(record.finishedStates.size(), 0);
    QVERIFY(!executor.isActive(id));

    const auto status = registry.status(id, kOwner, &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->state, ManifestState::Frozen);
}

/**
 * @brief cancelを2回要求すると2回目がAlreadyTerminalで拒否され、finishedが1回のみ発火することを検証する
 */
void TestRestorePlanExecutor::cancelTwiceIsRejectedAsAlreadyTerminal()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int total = RestorePlanExecutor::kEntriesPerExecutionChunk + 5;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));
    QVERIFY(scheduler.drainOne());
    QVERIFY(scheduler.queuedCount() > 0);

    QVERIFY(executor.requestCancel(id, kOwner, &error));
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Cancelled);

    QVERIFY(!executor.requestCancel(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));
    QCOMPARE(record.finishedStates.size(), 1);

    scheduler.drainAll();
    QCOMPARE(record.finishedStates.size(), 1);
}

/**
 * @brief cancel後のrequestContinueがAlreadyTerminalで拒否され、追加のapplier呼び出しを一切発生させないことを検証する
 */
void TestRestorePlanExecutor::continueAfterCancelDoesNotResumeWork()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int firstChunk = RestorePlanExecutor::kEntriesPerExecutionChunk;
    const int total = firstChunk + 9;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));
    QVERIFY(scheduler.drainOne());
    QCOMPARE(record.appliedPaths.size(), firstChunk);
    QVERIFY(scheduler.queuedCount() > 0);

    QVERIFY(executor.requestCancel(id, kOwner, &error));
    const int appliedAtCancel = record.appliedPaths.size();
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Cancelled);

    // まだqueueに残っていたchunkを排出しても、abandon/finishPlanで打ち切られているためapplierは増えない
    scheduler.drainAll();
    QCOMPARE(record.appliedPaths.size(), appliedAtCancel);

    QVERIFY(!executor.requestContinue(id, kOwner, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));
    QCOMPARE(record.appliedPaths.size(), appliedAtCancel);
    QCOMPARE(record.finishedStates.size(), 1);
    QVERIFY(!executor.isActive(id));
}

/**
 * @brief 実行中の計画がTTLを跨いで停滞しても回収されず、最後まで実行されることを検証する
 *
 * セキュリティ回帰ロック: 修正前はRunning計画もTTL満了で回収されたため、
 * 任意のローカルユーザがPolkitプロンプトを開いたまま放置してevent loopを
 * TTL超過までブロックするだけで、認可済みの復元が中途で黙って破棄され、
 * live filesystemが中途半端な状態のまま残っていた。
 */
void TestRestorePlanExecutor::planStalledPastTtlDuringExecutionKeepsRunning()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int total = 200;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    QVERIFY(scheduler.drainOne());
    QCOMPARE(record.appliedPaths.size(),
             RestorePlanExecutor::kEntriesPerExecutionChunk);
    QVERIFY(scheduler.queuedCount() > 0);

    // 別クライアントのPolkitプロンプトでchunkの進行がTTLを超えて停滞した状況
    nowMs += RestoreManifestRegistry::kDefaultTtlMs + 1;

    QVERIFY(scheduler.drainOne());

    QVERIFY(record.finishedStates.isEmpty());
    QCOMPARE(record.appliedPaths.size(),
             RestorePlanExecutor::kEntriesPerExecutionChunk * 2);
    QVERIFY(executor.isActive(id));

    // 停滞後も残りのentryを最後まで適用してCompletedへ到達する
    scheduler.drainAll();
    QCOMPARE(record.appliedPaths.size(), total);
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Completed);
    QVERIFY(!executor.isActive(id));
}

/**
 * @brief 実行中の計画がregistryから消滅した場合、Failed終端を必ず一度通知することを検証する
 *
 * F2欠陥の回帰ロック: 修正前はfinishPlan()を経由せずbareなm_activePlans.remove(id)で
 * 無言破棄されるため、finishedStatesが空のままとなりGUIが復元中のまま固まっていた。
 * owner消失時のremoveByOwner等でmanifestが消えた場合も終端通知は必ず発火する。
 */
void TestRestorePlanExecutor::vanishedPlanDuringExecutionEmitsFailedTerminal()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;
    configureExecutor(executor, scheduler, record);

    const int total = 200;
    const QString id = createFrozenPlan(registry, makePaths(total),
                                        makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    QVERIFY(scheduler.drainOne());
    QCOMPARE(record.appliedPaths.size(),
             RestorePlanExecutor::kEntriesPerExecutionChunk);
    QVERIFY(scheduler.queuedCount() > 0);

    QVERIFY(registry.remove(id));

    QVERIFY(scheduler.drainOne());

    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Failed);
    QCOMPARE(record.finishedMessages.size(), 1);
    QCOMPARE(record.finishedMessages.first(),
             QStringLiteral("Restore plan is no longer available"));
    QCOMPARE(record.appliedPaths.size(),
             RestorePlanExecutor::kEntriesPerExecutionChunk);
    QVERIFY(!executor.isActive(id));
}

/**
 * @brief entryごとのkeepAliveにより、単一chunkが長時間かかってもidle TTLが満了しないことを検証する
 *
 * applierがentryごとにclockを進めるため、修正前 (per-entry keepAliveが無い状態) では
 * 数entry適用しただけでTTLを超過しfinishVanishedPlan経由でFailedとなっていた。
 */
void TestRestorePlanExecutor::longRunningEntriesKeepPlanAlive()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    RestorePlanExecutor executor(registry);
    ManualScheduler scheduler;
    CallbackRecord record;

    executor.setChunkScheduler(
        [&scheduler](std::function<void()> chunk) {
            scheduler.enqueue(std::move(chunk));
        });
    executor.setEntryApplier(
        [&record, &nowMs](const QString &id, const RestoreEntry &entry) {
            Q_UNUSED(id)
            record.appliedPaths.append(entry.path);
            // 単一entryの適用がTTLの半分ほどかかる状況を模擬する
            nowMs += RestoreManifestRegistry::kDefaultTtlMs / 2;
            return true;
        });
    executor.setProgressSink(
        [&record](const QString &id, int current, int total,
                  const QString &path) {
            Q_UNUSED(id)
            record.progressCurrent.append(current);
            record.progressTotal.append(total);
            record.progressPaths.append(path);
        });
    executor.setFinishedSink(
        [&record](const QString &id, ManifestState terminal,
                  const QString &message) {
            Q_UNUSED(id)
            record.finishedStates.append(terminal);
            record.finishedMessages.append(message);
        });

    // kEntriesPerExecutionChunk + 36 = 100件: chunk境界も跨ぐ構成
    const int total = RestorePlanExecutor::kEntriesPerExecutionChunk + 36;
    const QStringList paths = makePaths(total);
    const QString id = createFrozenPlan(registry, paths, makeChangeTypes(total));
    QVERIFY(!id.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(executor.start(id, kOwner, &error));

    scheduler.drainAll();

    QCOMPARE(record.appliedPaths, paths);
    QCOMPARE(record.finishedStates.size(), 1);
    QCOMPARE(record.finishedStates.first(), ManifestState::Completed);
    QCOMPARE(record.finishedMessages.size(), 1);
    QCOMPARE(record.finishedMessages.first(), QStringLiteral("Restore completed"));
}

QTEST_APPLESS_MAIN(TestRestorePlanExecutor)
#include "tst_restoreplanexecutor.moc"
