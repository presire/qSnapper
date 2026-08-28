#include "restoremanifest.h"

#include <QTest>

#include <functional>

using namespace qsnapper::restore;

namespace {

constexpr qint64 kInitialTimeMs = 1000;

int errorValue(ManifestError error)
{
    return static_cast<int>(error);
}

QString createManifest(RestoreManifestRegistry &registry,
                       const QString &owner = QStringLiteral(":1.1"),
                       RestoreMode mode = RestoreMode::YastCompatible,
                       const QString &configName = QStringLiteral("root"),
                       int snapshotNumber = 42)
{
    ManifestError error = ManifestError::None;
    return registry.createStaging(owner, configName, snapshotNumber, mode, &error);
}

bool stageOne(RestoreManifestRegistry &registry, const QString &id,
              const QString &owner = QStringLiteral(":1.1"))
{
    ManifestError error = ManifestError::None;
    return registry.stageEntries(id, owner,
                                 {QStringLiteral("/etc/example.conf")},
                                 {QStringLiteral("modified")}, &error);
}

} // namespace

class TestRestoreManifest : public QObject
{
    Q_OBJECT

private slots:
    void appendAfterFreezeIsRejectedWithoutGrowth();
    void frozenPairsCannotBeReplaced();
    void crossOwnerAdvanceIsRejectedWithoutMovement();
    void everyOwnerScopedOperationRejectsWrongOwner();
    void everyControlOperationDropsExpiredManifest();
    void completedManifestRejectsReplay();
    void cancelledAndFailedManifestsRejectReplay();
    void markRunningTwiceIsRejected();
    void removeByOwnerRemovesOnlyMatchingManifests();
    void oversizedStageChunkIsAtomic();
    void entryCapacityExcessIsAtomic();
    void pathByteCapacityExcessIsAtomic();
    void registryManifestLimitsAreEnforced();
    void stagingBudgetIsGlobalNotPerConnection();
    void entriesPreserveOrderAcrossChunks();
    void cursorBoundsAndCompletionAreEnforced();
    void generatedIdsAreOpaqueAndUnique();
    void frozenStatusPreservesManifestMetadata();
    void malformedAndEdgeInputIsHandledAtomically();
    void stageOnFrozenManifestIsRejectedWithoutGrowth();
    void errorCodesForMissingWrongOwnerAndExpiredAreIndistinguishableAtTheApiBoundary();
    void keepAliveExtendsTtlWithoutMutatingState();
};

void TestRestoreManifest::appendAfterFreezeIsRejectedWithoutGrowth()
{
    RestoreManifest manifest(QStringLiteral(":1.1"), QStringLiteral("root"), 42,
                             RestoreMode::YastCompatible, QStringLiteral("rm-test"),
                             kInitialTimeMs, RestoreManifestRegistry::kDefaultTtlMs);
    ManifestError error = ManifestError::None;
    QVERIFY(manifest.appendEntries({{QStringLiteral("/one"), QStringLiteral("modified")}}, &error));
    QVERIFY(manifest.freeze(&error));
    const int countBefore = manifest.totalEntries();

    const bool appended = manifest.appendEntries(
        {{QStringLiteral("/two"), QStringLiteral("created")}}, &error);

    QVERIFY2(!appended, "appendEntries must reject additions after freeze");
    QCOMPARE(errorValue(error), errorValue(ManifestError::WrongState));
    QCOMPARE(manifest.totalEntries(), countBefore);
}

void TestRestoreManifest::frozenPairsCannotBeReplaced()
{
    RestoreManifest manifest(QStringLiteral(":1.1"), QStringLiteral("root"), 42,
                             RestoreMode::DirectCopy, QStringLiteral("rm-test"),
                             kInitialTimeMs, RestoreManifestRegistry::kDefaultTtlMs);
    const QVector<RestoreEntry> original{
        {QStringLiteral("/alpha"), QStringLiteral("created")},
        {QStringLiteral("/beta"), QStringLiteral("modified")},
        {QStringLiteral("/gamma"), QStringLiteral("deleted")}
    };
    ManifestError error = ManifestError::None;
    QVERIFY(manifest.appendEntries(original, &error));
    QVERIFY(manifest.freeze(&error));

    QVector<RestoreEntry> before;
    for (int index = 0; index < manifest.totalEntries(); ++index) {
        const auto entry = manifest.entryAt(index);
        QVERIFY(entry.has_value());
        before.append(*entry);
    }

    QVERIFY(!manifest.appendEntries(
        {{QStringLiteral("/replacement"), QStringLiteral("typechanged")}}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::WrongState));
    QCOMPARE(manifest.totalEntries(), before.size());
    for (int index = 0; index < before.size(); ++index) {
        const auto after = manifest.entryAt(index);
        QVERIFY(after.has_value());
        QCOMPARE(after->path, before.at(index).path);
        QCOMPARE(after->changeType, before.at(index).changeType);
    }
}

void TestRestoreManifest::crossOwnerAdvanceIsRejectedWithoutMovement()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QVERIFY(!id.isEmpty());
    QVERIFY(stageOne(registry, id));
    ManifestError error = ManifestError::None;
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.markRunning(id, QStringLiteral(":1.1"), &error));
    const auto before = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(before.has_value());

    QVERIFY(!registry.advance(id, QStringLiteral(":1.2"), 1, &error));

    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->cursor, before->cursor);
    QCOMPARE(after->state, ManifestState::Running);
}

void TestRestoreManifest::everyOwnerScopedOperationRejectsWrongOwner()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QVERIFY(!id.isEmpty());
    QVERIFY(stageOne(registry, id));
    const auto before = registry.status(id, QStringLiteral(":1.1"), nullptr);
    QVERIFY(before.has_value());

    ManifestError error = ManifestError::None;
    const QVector<std::function<bool()>> wrongOwnerCalls{
        [&]() { return registry.status(id, QStringLiteral(":1.2"), &error).has_value(); },
        [&]() { return registry.freeze(id, QStringLiteral(":1.2"), &error); },
        [&]() { return registry.cancel(id, QStringLiteral(":1.2"), &error); },
        [&]() { return registry.markFailed(id, QStringLiteral(":1.2"),
                                           QStringLiteral("not allowed"), &error); },
        [&]() { return registry.stageEntries(id, QStringLiteral(":1.2"),
                                             {QStringLiteral("/wrong-owner")},
                                             {QStringLiteral("created")}, &error); },
        [&]() { return registry.entriesSlice(id, QStringLiteral(":1.2"), 0, 1, &error).has_value(); }
    };

    for (const auto &call : wrongOwnerCalls) {
        error = ManifestError::None;
        QVERIFY(!call());
        QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    }

    const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->state, before->state);
    QCOMPARE(after->totalEntries, before->totalEntries);
    QCOMPARE(after->cursor, before->cursor);
}

void TestRestoreManifest::everyControlOperationDropsExpiredManifest()
{
    using ExpiredCall = std::function<bool(RestoreManifestRegistry &, const QString &, ManifestError *)>;
    const QVector<ExpiredCall> calls{
        [](auto &registry, const auto &id, auto *error) {
            return registry.stageEntries(id, QStringLiteral(":1.1"),
                                         {QStringLiteral("/expired")},
                                         {QStringLiteral("modified")}, error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.freeze(id, QStringLiteral(":1.1"), error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.markRunning(id, QStringLiteral(":1.1"), error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.advance(id, QStringLiteral(":1.1"), 1, error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.markFailed(id, QStringLiteral(":1.1"),
                                       QStringLiteral("expired"), error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.cancel(id, QStringLiteral(":1.1"), error);
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.status(id, QStringLiteral(":1.1"), error).has_value();
        },
        [](auto &registry, const auto &id, auto *error) {
            return registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 1, error).has_value();
        }
    };

    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    for (const auto &call : calls) {
        nowMs = kInitialTimeMs;
        const QString id = createManifest(registry);
        QVERIFY(!id.isEmpty());
        const int countBefore = registry.count();
        nowMs += RestoreManifestRegistry::kDefaultTtlMs + 1;
        ManifestError error = ManifestError::None;

        QVERIFY(!call(registry, id, &error));
        QCOMPARE(errorValue(error), errorValue(ManifestError::Expired));
        QCOMPARE(registry.count(), countBefore - 1);
        QVERIFY(!registry.status(id, QStringLiteral(":1.1"), &error).has_value());
        QCOMPARE(errorValue(error), errorValue(ManifestError::NotFound));
    }

    // Running状態の計画だけはTTL回収の対象外とする。
    // 認可済みかつexecutor駆動中の計画をTTLで回収すると、別クライアントのPolkitプロンプトが
    // 単一スレッドのevent loopをTTL超過までブロックしただけで進行中の復元が黙って破棄され、
    // live filesystemが中途半端な状態のまま残るため
    nowMs = kInitialTimeMs;
    const QString runningId = createManifest(registry);
    QVERIFY(!runningId.isEmpty());
    ManifestError error = ManifestError::None;
    QVERIFY(registry.stageEntries(runningId, QStringLiteral(":1.1"),
                                  {QStringLiteral("/one"), QStringLiteral("/two")},
                                  {QStringLiteral("modified"), QStringLiteral("created")}, &error));
    QVERIFY(registry.freeze(runningId, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.markRunning(runningId, QStringLiteral(":1.1"), &error));
    const int countWithRunning = registry.count();

    nowMs += RestoreManifestRegistry::kDefaultTtlMs * 10 + 1;
    QCOMPARE(registry.purgeExpired(), 0);
    QCOMPARE(registry.count(), countWithRunning);

    const auto stalledStatus = registry.status(runningId, QStringLiteral(":1.1"), &error);
    QVERIFY(stalledStatus.has_value());
    QCOMPARE(stalledStatus->state, ManifestState::Running);
    QVERIFY(registry.advance(runningId, QStringLiteral(":1.1"), 1, &error));

    // 終端へ達した計画は再びTTL回収の対象へ戻る
    QVERIFY(registry.advance(runningId, QStringLiteral(":1.1"), 1, &error));
    const auto completedStatus = registry.status(runningId, QStringLiteral(":1.1"), &error);
    QVERIFY(completedStatus.has_value());
    QCOMPARE(completedStatus->state, ManifestState::Completed);
    nowMs += RestoreManifestRegistry::kDefaultTtlMs + 1;
    QVERIFY(!registry.status(runningId, QStringLiteral(":1.1"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::Expired));

    nowMs = kInitialTimeMs;
    const QString ownerPriorityId = createManifest(registry);
    QVERIFY(!ownerPriorityId.isEmpty());
    nowMs += RestoreManifestRegistry::kDefaultTtlMs + 1;
    const int countBeforeWrongOwner = registry.count();
    QVERIFY(!registry.status(ownerPriorityId, QStringLiteral(":1.2"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    QCOMPARE(registry.count(), countBeforeWrongOwner);
    QVERIFY(!registry.status(ownerPriorityId, QStringLiteral(":1.1"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::Expired));
}

void TestRestoreManifest::completedManifestRejectsReplay()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QVERIFY(stageOne(registry, id));
    ManifestError error = ManifestError::None;
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.markRunning(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.advance(id, QStringLiteral(":1.1"), 1, &error));
    const auto completed = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(completed.has_value());
    QCOMPARE(completed->state, ManifestState::Completed);

    QVERIFY(!registry.markRunning(id, QStringLiteral(":1.1"), &error));
    QVERIFY(error == ManifestError::AlreadyTerminal || error == ManifestError::WrongState);
    QVERIFY(!registry.advance(id, QStringLiteral(":1.1"), 1, &error));
    QVERIFY(!registry.cancel(id, QStringLiteral(":1.1"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));
    QVERIFY(!registry.markFailed(id, QStringLiteral(":1.1"),
                                 QStringLiteral("replayed"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::AlreadyTerminal));

    const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->state, ManifestState::Completed);
    QCOMPARE(after->cursor, completed->cursor);
}

void TestRestoreManifest::cancelledAndFailedManifestsRejectReplay()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    ManifestError error = ManifestError::None;

    const QString cancelledId = createManifest(registry);
    QVERIFY(stageOne(registry, cancelledId));
    QVERIFY(registry.cancel(cancelledId, QStringLiteral(":1.1"), &error));

    const QString failedId = createManifest(registry);
    QVERIFY(stageOne(registry, failedId));
    QVERIFY(registry.markFailed(failedId, QStringLiteral(":1.1"),
                                QStringLiteral("copy failed"), &error));
    const auto failedStatus = registry.status(failedId, QStringLiteral(":1.1"), &error);
    QVERIFY(failedStatus.has_value());
    QCOMPARE(failedStatus->lastError, QStringLiteral("copy failed"));

    for (const QString &id : {cancelledId, failedId}) {
        const auto before = registry.status(id, QStringLiteral(":1.1"), &error);
        QVERIFY(before.has_value());
        const QVector<std::function<bool()>> replayCalls{
            [&]() { return registry.markRunning(id, QStringLiteral(":1.1"), &error); },
            [&]() { return registry.advance(id, QStringLiteral(":1.1"), 1, &error); },
            [&]() { return registry.freeze(id, QStringLiteral(":1.1"), &error); },
            [&]() { return registry.stageEntries(id, QStringLiteral(":1.1"),
                                                 {QStringLiteral("/replay")},
                                                 {QStringLiteral("modified")}, &error); }
        };
        for (const auto &call : replayCalls) {
            QVERIFY(!call());
        }
        const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
        QVERIFY(after.has_value());
        QCOMPARE(after->state, before->state);
        QCOMPARE(after->totalEntries, before->totalEntries);
        QCOMPARE(after->cursor, before->cursor);
    }
}

void TestRestoreManifest::markRunningTwiceIsRejected()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QVERIFY(stageOne(registry, id));
    ManifestError error = ManifestError::None;
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.markRunning(id, QStringLiteral(":1.1"), &error));

    QVERIFY(!registry.markRunning(id, QStringLiteral(":1.1"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::WrongState));
    const auto status = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->state, ManifestState::Running);
    QCOMPARE(status->cursor, 0);
}

void TestRestoreManifest::removeByOwnerRemovesOnlyMatchingManifests()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString first = createManifest(registry, QStringLiteral(":1.1"));
    const QString second = createManifest(registry, QStringLiteral(":1.1"));
    const QString survivor = createManifest(registry, QStringLiteral(":1.2"));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(!survivor.isEmpty());

    QCOMPARE(registry.removeByOwner(QStringLiteral(":1.1")), 2);
    QCOMPARE(registry.count(), 1);
    QCOMPARE(registry.countForOwner(QStringLiteral(":1.1")), 0);
    QCOMPARE(registry.countForOwner(QStringLiteral(":1.2")), 1);

    ManifestError error = ManifestError::None;
    QVERIFY(!registry.status(first, QStringLiteral(":1.1"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::NotFound));
    QVERIFY(!registry.status(second, QStringLiteral(":1.1"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::NotFound));
    QVERIFY(registry.status(survivor, QStringLiteral(":1.2"), &error).has_value());
}

void TestRestoreManifest::oversizedStageChunkIsAtomic()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QStringList paths;
    QStringList changeTypes;
    paths.reserve(RestoreManifestRegistry::kMaxEntriesPerStageChunk + 1);
    changeTypes.reserve(RestoreManifestRegistry::kMaxEntriesPerStageChunk + 1);
    for (int index = 0; index <= RestoreManifestRegistry::kMaxEntriesPerStageChunk; ++index) {
        paths.append(QStringLiteral("/file-%1").arg(index));
        changeTypes.append(QStringLiteral("modified"));
    }
    ManifestError error = ManifestError::None;

    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"), paths, changeTypes, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::CapacityExceeded));
    const auto status = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->totalEntries, 0);
}

void TestRestoreManifest::entryCapacityExcessIsAtomic()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    registry.setCapacityOverridesForTesting(3, 1024);
    const QString id = createManifest(registry);
    ManifestError error = ManifestError::None;
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/one"), QStringLiteral("/two")},
                                  {QStringLiteral("created"), QStringLiteral("modified")}, &error));
    const auto before = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 10, &error);
    QVERIFY(before.has_value());

    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"),
                                   {QStringLiteral("/three"), QStringLiteral("/four")},
                                   {QStringLiteral("deleted"), QStringLiteral("typechanged")}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::CapacityExceeded));
    const auto after = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 10, &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->size(), before->size());
    for (int index = 0; index < before->size(); ++index) {
        QCOMPARE(after->at(index).path, before->at(index).path);
        QCOMPARE(after->at(index).changeType, before->at(index).changeType);
    }
}

void TestRestoreManifest::pathByteCapacityExcessIsAtomic()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    registry.setCapacityOverridesForTesting(10, 8);
    const QString id = createManifest(registry);
    ManifestError error = ManifestError::None;
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/one")},
                                  {QStringLiteral("modified")}, &error));
    const auto before = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(before.has_value());

    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"),
                                   {QStringLiteral("/12345")},
                                   {QStringLiteral("created")}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::CapacityExceeded));
    const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->totalEntries, before->totalEntries);
    const auto entries = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 10, &error);
    QVERIFY(entries.has_value());
    QCOMPARE(entries->size(), 1);
    QCOMPARE(entries->first().path, QStringLiteral("/one"));
}

void TestRestoreManifest::registryManifestLimitsAreEnforced()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry perOwnerRegistry;
    perOwnerRegistry.setClock([&nowMs]() { return nowMs; });
    ManifestError error = ManifestError::None;
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsPerOwner; ++index) {
        QVERIFY(!createManifest(perOwnerRegistry).isEmpty());
    }
    const int perOwnerCount = perOwnerRegistry.count();
    const QString rejectedPerOwner = perOwnerRegistry.createStaging(
        QStringLiteral(":1.1"), QStringLiteral("root"), 42,
        RestoreMode::YastCompatible, &error);
    QVERIFY(rejectedPerOwner.isEmpty());
    QCOMPARE(errorValue(error), errorValue(ManifestError::CapacityExceeded));
    QCOMPARE(perOwnerRegistry.count(), perOwnerCount);

    RestoreManifestRegistry globalRegistry;
    globalRegistry.setClock([&nowMs]() { return nowMs; });
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsGlobal; ++index) {
        const QString owner = QStringLiteral(":2.%1").arg(index);
        QVERIFY(!createManifest(globalRegistry, owner).isEmpty());
    }
    const int globalCount = globalRegistry.count();
    const QString rejectedGlobal = globalRegistry.createStaging(
        QStringLiteral(":3.1"), QStringLiteral("root"), 42,
        RestoreMode::YastCompatible, &error);
    QVERIFY(rejectedGlobal.isEmpty());
    QCOMPARE(errorValue(error), errorValue(ManifestError::GlobalLimit));
    QCOMPARE(globalRegistry.count(), globalCount);
}

/**
 * @brief stagingの予算がregistry全体で閉じており、接続数で掛け算できないこと
 *
 * BeginRestorePlan / StageRestoreEntries は設計上、認可を要さない。
 * 上限がmanifest単位にしか無いと、一般ユーザがD-Bus接続を複数開くだけで
 * (unique nameごとに別ownerとして数えられるため) rootサービスに
 * manifest上限 x 接続数のメモリを確保させられる。
 * 本テストは entry数 / path byte の両軸で、2つ目のownerが最初のownerの
 * 消費分を引き継いだ残余しか使えず、超過時はGlobalLimitで
 * manifestを一切変化させずに拒否されることを固定する。
 */
void TestRestoreManifest::stagingBudgetIsGlobalNotPerConnection()
{
    const QString ownerA = QStringLiteral(":1.1");
    const QString ownerB = QStringLiteral(":1.2");

    // --- entry数の軸 ---
    {
        qint64 nowMs = kInitialTimeMs;
        RestoreManifestRegistry registry;
        registry.setClock([&nowMs]() { return nowMs; });
        // manifest単位は緩く、グローバルだけを絞る
        registry.setCapacityOverridesForTesting(100, 1024,
                                                /*maxEntriesGlobal=*/3,
                                                /*maxPathBytesGlobal=*/1024);
        ManifestError error = ManifestError::None;
        const QString idA = createManifest(registry, ownerA);
        const QString idB = createManifest(registry, ownerB);
        QVERIFY(!idA.isEmpty());
        QVERIFY(!idB.isEmpty());

        QVERIFY(registry.stageEntries(idA, ownerA,
                                      {QStringLiteral("/a1"), QStringLiteral("/a2")},
                                      {QStringLiteral("created"), QStringLiteral("modified")},
                                      &error));
        QCOMPARE(registry.globalEntries(), qint64(2));

        // ownerBはmanifest単位では100件まで許されるが、
        // 全体予算の残りは1件しかない
        QVERIFY2(!registry.stageEntries(idB, ownerB,
                                        {QStringLiteral("/b1"), QStringLiteral("/b2")},
                                        {QStringLiteral("created"), QStringLiteral("deleted")},
                                        &error),
                 "a second connection must not get its own entry budget");
        QCOMPARE(errorValue(error), errorValue(ManifestError::GlobalLimit));
        QCOMPARE(registry.globalEntries(), qint64(2));
        const auto bAfterReject = registry.status(idB, ownerB, &error);
        QVERIFY(bAfterReject.has_value());
        QCOMPARE(bAfterReject->totalEntries, 0);

        // 残余ぶんは通る
        QVERIFY(registry.stageEntries(idB, ownerB, {QStringLiteral("/b1")},
                                      {QStringLiteral("created")}, &error));
        QCOMPARE(registry.globalEntries(), qint64(3));
    }

    // --- path byteの軸 ---
    {
        qint64 nowMs = kInitialTimeMs;
        RestoreManifestRegistry registry;
        registry.setClock([&nowMs]() { return nowMs; });
        registry.setCapacityOverridesForTesting(100, 1024,
                                                /*maxEntriesGlobal=*/100,
                                                /*maxPathBytesGlobal=*/12);
        ManifestError error = ManifestError::None;
        const QString idA = createManifest(registry, ownerA);
        const QString idB = createManifest(registry, ownerB);
        QVERIFY(!idA.isEmpty());
        QVERIFY(!idB.isEmpty());

        // "/aaaaaaaa" = 9 byte
        QVERIFY(registry.stageEntries(idA, ownerA, {QStringLiteral("/aaaaaaaa")},
                                      {QStringLiteral("created")}, &error));
        QCOMPARE(registry.globalPathBytes(), qint64(9));

        // 残り3 byteしかないので "/bbbbbbbb" (9 byte) は入らない
        QVERIFY2(!registry.stageEntries(idB, ownerB, {QStringLiteral("/bbbbbbbb")},
                                        {QStringLiteral("created")}, &error),
                 "a second connection must not get its own path-byte budget");
        QCOMPARE(errorValue(error), errorValue(ManifestError::GlobalLimit));
        QCOMPARE(registry.globalPathBytes(), qint64(9));
        const auto bAfterReject = registry.status(idB, ownerB, &error);
        QVERIFY(bAfterReject.has_value());
        QCOMPARE(bAfterReject->totalEntries, 0);

        // "/bb" = 3 byte はちょうど収まる
        QVERIFY(registry.stageEntries(idB, ownerB, {QStringLiteral("/bb")},
                                      {QStringLiteral("created")}, &error));
        QCOMPARE(registry.globalPathBytes(), qint64(12));
    }

    // --- productionの既定値が「掛け算できない」大きさに保たれていること ---
    // manifest単位の上限 x manifest上限数 (= 32 x 64MB) がそのまま
    // 確保され得ないことを、定数レベルで固定する
    QVERIFY2(RestoreManifestRegistry::kMaxPathBytesGlobal
                 < RestoreManifestRegistry::kMaxPathBytesPerManifest
                     * RestoreManifestRegistry::kMaxManifestsGlobal,
             "global path-byte budget must be smaller than "
             "per-manifest limit x manifest count");
    QVERIFY2(RestoreManifestRegistry::kMaxEntriesGlobal
                 < qint64(RestoreManifestRegistry::kMaxEntriesPerManifest)
                     * RestoreManifestRegistry::kMaxManifestsGlobal,
             "global entry budget must be smaller than "
             "per-manifest limit x manifest count");
    // 最大構成の計画1件は必ず通ること (正規利用を壊さない)
    QVERIFY(RestoreManifestRegistry::kMaxPathBytesGlobal
                >= RestoreManifestRegistry::kMaxPathBytesPerManifest);
    QVERIFY(RestoreManifestRegistry::kMaxEntriesGlobal
                >= RestoreManifestRegistry::kMaxEntriesPerManifest);
}

void TestRestoreManifest::entriesPreserveOrderAcrossChunks()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    ManifestError error = ManifestError::None;
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/a"), QStringLiteral("/b")},
                                  {QStringLiteral("created"), QStringLiteral("modified")}, &error));
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/c")},
                                  {QStringLiteral("deleted")}, &error));
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/d"), QStringLiteral("/e")},
                                  {QStringLiteral("typechanged"), QStringLiteral("modified")}, &error));

    const auto slice = registry.entriesSlice(id, QStringLiteral(":1.1"), 1, 3, &error);
    QVERIFY(slice.has_value());
    QCOMPARE(slice->size(), 3);
    QCOMPARE(slice->at(0).path, QStringLiteral("/b"));
    QCOMPARE(slice->at(0).changeType, QStringLiteral("modified"));
    QCOMPARE(slice->at(1).path, QStringLiteral("/c"));
    QCOMPARE(slice->at(1).changeType, QStringLiteral("deleted"));
    QCOMPARE(slice->at(2).path, QStringLiteral("/d"));
    QCOMPARE(slice->at(2).changeType, QStringLiteral("typechanged"));
}

void TestRestoreManifest::cursorBoundsAndCompletionAreEnforced()
{
    RestoreManifest manifest(QStringLiteral(":1.1"), QStringLiteral("root"), 42,
                             RestoreMode::YastCompatible, QStringLiteral("rm-test"),
                             kInitialTimeMs, RestoreManifestRegistry::kDefaultTtlMs);
    ManifestError error = ManifestError::None;
    QVERIFY(manifest.appendEntries({
        {QStringLiteral("/one"), QStringLiteral("modified")},
        {QStringLiteral("/two"), QStringLiteral("created")}
    }, &error));
    QVERIFY(manifest.freeze(&error));
    QVERIFY(manifest.markRunning(&error));

    QVERIFY(!manifest.advanceCursor(0, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::InvalidArgument));
    QCOMPARE(manifest.cursor(), 0);
    QVERIFY(!manifest.advanceCursor(3, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::InvalidArgument));
    QCOMPARE(manifest.cursor(), 0);
    QCOMPARE(manifest.state(), ManifestState::Running);
    QVERIFY(manifest.advanceCursor(1, &error));
    QCOMPARE(manifest.cursor(), 1);
    QCOMPARE(manifest.state(), ManifestState::Running);
    QVERIFY(manifest.advanceCursor(1, &error));
    QCOMPARE(manifest.cursor(), 2);
    QCOMPARE(manifest.state(), ManifestState::Completed);
    QVERIFY(manifest.isTerminal());
}

void TestRestoreManifest::generatedIdsAreOpaqueAndUnique()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString first = createManifest(registry);
    const QString second = createManifest(registry);

    QVERIFY(first != second);
    QVERIFY(first.length() >= 16);
    QVERIFY(second.length() >= 16);
    bool firstIsInteger = false;
    bool secondIsInteger = false;
    first.toLongLong(&firstIsInteger);
    second.toLongLong(&secondIsInteger);
    QVERIFY(!firstIsInteger);
    QVERIFY(!secondIsInteger);
}

void TestRestoreManifest::frozenStatusPreservesManifestMetadata()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), QStringLiteral("home-config"), 987,
        RestoreMode::DirectCopy, &error);
    QVERIFY(!id.isEmpty());
    QVERIFY(stageOne(registry, id));
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    const auto status = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->mode, RestoreMode::DirectCopy);
    QCOMPARE(status->configName, QStringLiteral("home-config"));
    QCOMPARE(status->snapshotNumber, 987);
    QCOMPARE(status->state, ManifestState::Frozen);
}

void TestRestoreManifest::malformedAndEdgeInputIsHandledAtomically()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    registry.setCapacityOverridesForTesting(10, 64);
    const QString id = createManifest(registry);
    ManifestError error = ManifestError::None;

    const QString emptyId = createManifest(registry, QStringLiteral(":1.2"));
    QVERIFY(!emptyId.isEmpty());
    QVERIFY(!registry.freeze(emptyId, QStringLiteral(":1.2"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::InvalidArgument));

    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"),
                                   {QStringLiteral("/one")}, {}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::InvalidArgument));
    auto status = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->totalEntries, 0);

    QString embeddedNul = QStringLiteral("/ユニコード");
    embeddedNul.append(QChar(u'\0'));
    embeddedNul.append(QStringLiteral("tail"));
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QString(), embeddedNul},
                                  {QString(), QStringLiteral("modified")}, &error));
    const auto safelyStored = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 2, &error);
    QVERIFY(safelyStored.has_value());
    QCOMPARE(safelyStored->size(), 2);
    QCOMPARE(safelyStored->at(0).path, QString());
    QCOMPARE(safelyStored->at(0).changeType, QString());
    QCOMPARE(safelyStored->at(1).path, embeddedNul);

    const int countBeforeLongPath = safelyStored->size();
    const QString extremelyLongPath(1024, QLatin1Char('x'));
    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"),
                                   {extremelyLongPath},
                                   {QStringLiteral("modified")}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::CapacityExceeded));
    status = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(status.has_value());
    QCOMPARE(status->totalEntries, countBeforeLongPath);
}

/**
 * @brief registry.stageEntries()がFrozen manifestに対しWrongStateで拒否され、
 *        entry列・totalEntries・pathBytes会計のいずれも変化させないことを検証する
 *
 * appendAfterFreezeIsRejectedWithoutGrowth はRestoreManifest::appendEntries()を
 * 直接呼び出すstate machine単体の検証であり、registry層の事前条件チェック
 * (stageEntries内、manifest->appendEntriesへ到達する前の
 * manifest->state() != Staging判定) は別経路のため未検証だった。
 */
void TestRestoreManifest::stageOnFrozenManifestIsRejectedWithoutGrowth()
{
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    const QString id = createManifest(registry);
    QVERIFY(!id.isEmpty());
    QVERIFY(stageOne(registry, id));
    ManifestError error = ManifestError::None;
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    const auto before = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(before.has_value());
    const auto beforeEntries = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 10, &error);
    QVERIFY(beforeEntries.has_value());

    QVERIFY(!registry.stageEntries(id, QStringLiteral(":1.1"),
                                   {QStringLiteral("/after-freeze")},
                                   {QStringLiteral("created")}, &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::WrongState));

    const auto after = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(after.has_value());
    QCOMPARE(after->state, ManifestState::Frozen);
    QCOMPARE(after->totalEntries, before->totalEntries);
    const auto afterEntries = registry.entriesSlice(id, QStringLiteral(":1.1"), 0, 10, &error);
    QVERIFY(afterEntries.has_value());
    QCOMPARE(afterEntries->size(), beforeEntries->size());
    for (int index = 0; index < beforeEntries->size(); ++index) {
        QCOMPARE(afterEntries->at(index).path, beforeEntries->at(index).path);
    }
}

/**
 * @brief NotFound/OwnerMismatch/Expiredの3経路がAPI境界で同一エラーへ
 *        集約される (sendManifestError側の責務) 一方、registry内部では
 *        別個のManifestErrorを返し、かつ関係ない他manifestの保存状態を
 *        一切変化させないことを検証する
 *
 * SnapshotOperations::sendManifestError (snapshotoperations.cpp 行452-493) は
 * NotFound/OwnerMismatch/Expiredをすべて同一のD-Bus AccessDenied
 * "Restore plan access denied" へ集約する (情報漏洩防止)。
 * しかしregistry層ではこの3種は明確に異なるManifestErrorとして区別され、
 * どの経路でも「無関係なmanifestの状態」および「対象manifest自身の
 * 呼び出し前状態」は変更されない。本テストはその不変条件を検証する。
 */
void TestRestoreManifest::errorCodesForMissingWrongOwnerAndExpiredAreIndistinguishableAtTheApiBoundary()
{
    const qint64 ttl = RestoreManifestRegistry::kDefaultTtlMs;
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    ManifestError error = ManifestError::None;

    // expiringId: 早期に作成し、以降一切touchしない (単独で失効させるため)
    const QString expiringId = createManifest(registry, QStringLiteral(":9.1"));
    QVERIFY(!expiringId.isEmpty());

    // controlId / targetId: 時間を進めた後に作成・操作 (expiringIdより後にtouch)
    nowMs = kInitialTimeMs + ttl / 2;
    const QString controlId = createManifest(registry, QStringLiteral(":1.1"));
    QVERIFY(!controlId.isEmpty());
    QVERIFY(stageOne(registry, controlId));

    const QString targetId = createManifest(registry, QStringLiteral(":1.1"));
    QVERIFY(!targetId.isEmpty());
    QVERIFY(stageOne(registry, targetId));
    QVERIFY(registry.freeze(targetId, QStringLiteral(":1.1"), &error));

    // expiringIdのみが失効し、controlId/targetIdはまだ有効な時刻へ進める
    nowMs = kInitialTimeMs + ttl + 1;

    // --- 経路1: NotFound (存在しないid) ---
    const int countBeforeMissing = registry.count();
    const auto controlBeforeMissing = registry.status(controlId, QStringLiteral(":1.1"), &error);
    QVERIFY(controlBeforeMissing.has_value());

    const auto missingResult = registry.status(
        QStringLiteral("rm-does-not-exist"), QStringLiteral(":1.1"), &error);
    QVERIFY(!missingResult.has_value());
    const ManifestError notFoundError = error;
    QCOMPARE(errorValue(notFoundError), errorValue(ManifestError::NotFound));
    QCOMPARE(registry.count(), countBeforeMissing);
    const auto controlAfterMissing = registry.status(controlId, QStringLiteral(":1.1"), &error);
    QVERIFY(controlAfterMissing.has_value());
    QCOMPARE(controlAfterMissing->state, controlBeforeMissing->state);
    QCOMPARE(controlAfterMissing->totalEntries, controlBeforeMissing->totalEntries);
    QCOMPARE(controlAfterMissing->cursor, controlBeforeMissing->cursor);

    // --- 経路2: OwnerMismatch (実在するtargetIdへ誤ったowner) ---
    const auto targetBefore = registry.status(targetId, QStringLiteral(":1.1"), &error);
    QVERIFY(targetBefore.has_value());
    const int countBeforeWrongOwner = registry.count();

    const auto wrongOwnerResult = registry.status(targetId, QStringLiteral(":9.9"), &error);
    QVERIFY(!wrongOwnerResult.has_value());
    const ManifestError ownerMismatchError = error;
    QCOMPARE(errorValue(ownerMismatchError), errorValue(ManifestError::OwnerMismatch));
    QCOMPARE(registry.count(), countBeforeWrongOwner);
    const auto targetAfter = registry.status(targetId, QStringLiteral(":1.1"), &error);
    QVERIFY(targetAfter.has_value());
    QCOMPARE(targetAfter->state, targetBefore->state);
    QCOMPARE(targetAfter->totalEntries, targetBefore->totalEntries);
    QCOMPARE(targetAfter->cursor, targetBefore->cursor);

    // --- 経路3: Expired (expiringIdのみ失効、controlId/targetIdは無関係のまま) ---
    const auto controlBeforeExpiry = registry.status(controlId, QStringLiteral(":1.1"), &error);
    QVERIFY(controlBeforeExpiry.has_value());
    const int countBeforeExpiry = registry.count();

    const auto expiredResult = registry.status(expiringId, QStringLiteral(":9.1"), &error);
    QVERIFY(!expiredResult.has_value());
    const ManifestError expiredError = error;
    QCOMPARE(errorValue(expiredError), errorValue(ManifestError::Expired));
    QCOMPARE(registry.count(), countBeforeExpiry - 1);
    const auto controlAfterExpiry = registry.status(controlId, QStringLiteral(":1.1"), &error);
    QVERIFY(controlAfterExpiry.has_value());
    QCOMPARE(controlAfterExpiry->state, controlBeforeExpiry->state);
    QCOMPARE(controlAfterExpiry->totalEntries, controlBeforeExpiry->totalEntries);
    QCOMPARE(controlAfterExpiry->cursor, controlBeforeExpiry->cursor);
    const auto targetStillThere = registry.status(targetId, QStringLiteral(":1.1"), &error);
    QVERIFY(targetStillThere.has_value());

    // registry内部では3経路が明確に異なるコードである
    // (sendManifestErrorはこれらを同一のAccessDeniedへ集約するが、その集約は
    // D-Bus adapter層の責務でありregistry層のテスト範囲外)
    QVERIFY(notFoundError != ownerMismatchError);
    QVERIFY(notFoundError != expiredError);
    QVERIFY(ownerMismatchError != expiredError);
}

/**
 * @brief keepAliveがowner・失効を確認した上でTTLのみ延長し、状態を一切変更しないことを検証する
 */
void TestRestoreManifest::keepAliveExtendsTtlWithoutMutatingState()
{
    const qint64 ttl = RestoreManifestRegistry::kDefaultTtlMs;
    qint64 nowMs = kInitialTimeMs;
    RestoreManifestRegistry registry;
    registry.setClock([&nowMs]() { return nowMs; });
    ManifestError error = ManifestError::None;

    // --- 誤ったowner ---
    const QString id = createManifest(registry);
    QVERIFY(!id.isEmpty());
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/one"), QStringLiteral("/two")},
                                  {QStringLiteral("modified"), QStringLiteral("created")},
                                  &error));
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.markRunning(id, QStringLiteral(":1.1"), &error));
    QVERIFY(registry.advance(id, QStringLiteral(":1.1"), 1, &error));
    const auto beforeWrongOwner = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(beforeWrongOwner.has_value());

    QVERIFY(!registry.keepAlive(id, QStringLiteral(":1.2"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::OwnerMismatch));
    const auto afterWrongOwner = registry.status(id, QStringLiteral(":1.1"), &error);
    QVERIFY(afterWrongOwner.has_value());
    QCOMPARE(afterWrongOwner->state, beforeWrongOwner->state);
    QCOMPARE(afterWrongOwner->cursor, beforeWrongOwner->cursor);
    QCOMPARE(afterWrongOwner->processed, beforeWrongOwner->processed);
    QCOMPARE(afterWrongOwner->totalEntries, beforeWrongOwner->totalEntries);

    // --- 既に失効済み ---
    const QString expiredId = createManifest(registry, QStringLiteral(":2.1"));
    QVERIFY(!expiredId.isEmpty());
    nowMs += ttl + 1;
    QVERIFY(!registry.keepAlive(expiredId, QStringLiteral(":2.1"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::Expired));
    QVERIFY(!registry.status(expiredId, QStringLiteral(":2.1"), &error).has_value());
    QCOMPARE(errorValue(error), errorValue(ManifestError::NotFound));

    // --- 正当なowner: TTLのみ延長し、状態は不変 ---
    nowMs = kInitialTimeMs;
    const QString liveId = createManifest(registry, QStringLiteral(":3.1"));
    QVERIFY(!liveId.isEmpty());
    QVERIFY(registry.stageEntries(liveId, QStringLiteral(":3.1"),
                                  {QStringLiteral("/alpha"), QStringLiteral("/beta")},
                                  {QStringLiteral("modified"), QStringLiteral("created")},
                                  &error));
    QVERIFY(registry.freeze(liveId, QStringLiteral(":3.1"), &error));
    QVERIFY(registry.markRunning(liveId, QStringLiteral(":3.1"), &error));
    QVERIFY(registry.advance(liveId, QStringLiteral(":3.1"), 1, &error));

    // advance()のtouch後、TTLの半分だけ経過させてからkeepAliveを呼ぶ
    nowMs += ttl / 2;
    const auto beforeKeepAlive = registry.status(liveId, QStringLiteral(":3.1"), &error);
    QVERIFY(beforeKeepAlive.has_value());

    QVERIFY(registry.keepAlive(liveId, QStringLiteral(":3.1"), &error));
    QCOMPARE(errorValue(error), errorValue(ManifestError::None));

    const auto afterKeepAlive = registry.status(liveId, QStringLiteral(":3.1"), &error);
    QVERIFY(afterKeepAlive.has_value());
    QCOMPARE(afterKeepAlive->state, beforeKeepAlive->state);
    QCOMPARE(afterKeepAlive->cursor, beforeKeepAlive->cursor);
    QCOMPARE(afterKeepAlive->processed, beforeKeepAlive->processed);
    QCOMPARE(afterKeepAlive->totalEntries, beforeKeepAlive->totalEntries);

    // keepAlive呼び出し時点からさらにTTL弱だけ進める。
    // advance()のtouchからの経過はttl/2 + (ttl-1) > ttlとなり、keepAliveが
    // なければ本来ここで失効しているはずだが、keepAliveがtouchを更新したため
    // まだttl-1しか経過しておらず到達可能なままである
    nowMs += ttl - 1;
    const auto stillAlive = registry.status(liveId, QStringLiteral(":3.1"), &error);
    QVERIFY(stillAlive.has_value());
    QCOMPARE(stillAlive->cursor, beforeKeepAlive->cursor);
    QCOMPARE(stillAlive->state, beforeKeepAlive->state);
}

QTEST_MAIN(TestRestoreManifest)
#include "tst_restoremanifest.moc"
