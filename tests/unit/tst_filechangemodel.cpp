#include <QtTest>
#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include "filechangemodel.h"

class TestableFileChangeModel : public FileChangeModel
{
public:
    struct PendingStringReply
    {
        QString methodName;
        QVariantList arguments;
        AsyncStringReplyHandler done;
    };

    using FileChangeModel::setupModelData;
    using FileChangeModel::setComparisonRange;

    QList<PendingStringReply> pendingStringReplies;

    void requestStringReply(const QString &methodName,
                            const QVariantList &arguments,
                            AsyncStringReplyHandler done) override
    {
        pendingStringReplies.append({methodName, arguments, done});
    }

    void completeStringReply(int index, bool ok, const QString &value = QString(),
                             const QString &error = QString())
    {
        Q_ASSERT(index >= 0 && index < pendingStringReplies.size());
        const PendingStringReply pending = pendingStringReplies.takeAt(index);
        pending.done(ok, value, error);
    }
};

/**
 * @brief RestorePlanTransportのテスト用モック
 *
 * 全ての呼び出しを記録し、コールバックの完了をテスト側から駆動できる
 * シグナル推送はsubscribePlanSignals()で登録されたreceiverのスロットをQMetaObject::invokeMethodで直接呼び出すことで模倣する
 */
class FakeRestorePlanTransport : public RestorePlanTransport
{
public:
    struct Call
    {
        enum Kind { Begin, Stage, Commit, Cancel, Status };
        Kind kind = Begin;
        QString configName;
        int snapshotNumber = 0;
        int counterpartNumber = 0;
        QString restoreMode;
        QString manifestId;
        QStringList paths;
        QStringList changeTypes;
    };

    QList<Call> calls;
    QString nextManifestId = QStringLiteral("manifest-1");
    QObject *receiver = nullptr;
    int subscribeCount = 0;
    int unsubscribeCount = 0;

    int countKind(Call::Kind kind) const
    {
        int count = 0;
        for (const Call &call : calls) {
            if (call.kind == kind) {
                ++count;
            }
        }
        return count;
    }

    bool hasPending() const { return !m_pending.isEmpty(); }

    void completePending(bool ok, const QString &value = QString(), const QString &error = QString())
    {
        if (m_pending.isEmpty()) {
            return;
        }
        const Pending pending = m_pending.takeFirst();
        pending.invoke(ok, value, error);
    }

    void completeAllPendingOk()
    {
        while (!m_pending.isEmpty()) {
            const Pending pending = m_pending.takeFirst();
            pending.invoke(true, pending.kind == Call::Begin ? nextManifestId : QString(), QString());
        }
    }

    void emitPlanProgress(const QString &manifestId, int current, int total, const QString &filePath)
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanProgress",
                                  Q_ARG(QString, manifestId), Q_ARG(int, current),
                                  Q_ARG(int, total), Q_ARG(QString, filePath));
    }

    void emitPlanFinished(const QString &manifestId, const QString &terminalState, const QString &message)
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanFinished",
                                  Q_ARG(QString, manifestId), Q_ARG(QString, terminalState),
                                  Q_ARG(QString, message));
    }

    /**
     * @brief 復元サービスの消失通知を送出する
     */
    void emitServiceVanished()
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanServiceVanished");
    }

    // --- RestorePlanTransport interface ---

    void beginPlan(const QString &configName, int snapshotNumber,
                   int counterpartSnapshotNumber, const QString &restoreMode,
                   std::function<void(bool ok, const QString &manifestId, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Begin;
        call.configName = configName;
        call.snapshotNumber = snapshotNumber;
        call.counterpartNumber = counterpartSnapshotNumber;
        call.restoreMode = restoreMode;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Begin;
        pending.invoke = [done](bool ok, const QString &value, const QString &error) {
            done(ok, value, error);
        };
        m_pending.append(pending);
    }

    void stageEntries(const QString &manifestId, const QStringList &paths, const QStringList &changeTypes,
                      std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Stage;
        call.manifestId = manifestId;
        call.paths = paths;
        call.changeTypes = changeTypes;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Stage;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void commitPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Commit;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Commit;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void cancelPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Cancel;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Cancel;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void requestStatus(const QString &manifestId,
                       std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Status;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Status;
        pending.invoke = [done](bool ok, const QString &value, const QString &error) {
            done(ok, value, error);
        };
        m_pending.append(pending);
    }

    bool subscribePlanSignals(QObject *receiver) override
    {
        this->receiver = receiver;
        ++subscribeCount;
        return true;
    }

    void unsubscribePlanSignals(QObject *receiver) override
    {
        if (this->receiver == receiver) {
            this->receiver = nullptr;
        }
        ++unsubscribeCount;
    }

private:
    struct Pending
    {
        Call::Kind kind = Call::Begin;
        std::function<void(bool ok, const QString &value, const QString &error)> invoke;
    };

    QList<Pending> m_pending;
};

class TestFileChangeModel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void treeBuildRetainsSpacesDeduplicatesAndMarksParents();
    void flatBuildPreservesInputOrderAndExplicitDirectories();
    void treeBuildHandlesPrefixHeavyPaths();
    void stagedRestoreStagesBoundedChunksAndCommitsOnce();
    void stagedRestoreSendsNoMutableValuesAfterCommit();
    void stagedRestoreProgressIsMonotonicOverOriginalTotal();
    void stagedRestoreCompletedTerminalEmitsCompletionOnce();
    void stagedRestoreServiceVanishAfterCommitFinishesWithFailure();
    void stagedRestoreServiceVanishDuringStagingFinishesWithFailure();
    void stagedRestoreStageFailureAbortsFlowWithError();
    void stagedRestoreCommitFailureAbortsFlowWithError();
    void stagedRestoreCancelWaitsForTerminalCancelled();
    void stagedRestoreIgnoresForeignManifestId();
    void stagedRestoreNormalizesTrailingSlashDirectory();
    void stagedRestoreFiltersInvalidEntriesBeforeStaging();
    void stagedRestoreCancelDuringStagingFinishesLocally();
    void prePostRestoreUsesComparisonSourceAndPreservesBetweenMode();
    void prePostRestoreFromComparisonTargetIsRejected();
    void beginPlanCarriesCounterpartFromLoadedComparisonInBothDirections();
    void beginPlanCarriesZeroCounterpartInVsCurrentMode();
    void syntheticParentWithEmptyStatusFlagsNeverReachesThePlan();
    void restoreFromMismatchedSnapshotIsRejectedInVsCurrentMode();
    void singleFileRestoreFromMismatchedSnapshotIsRejected();
    void failedComparisonReplacementClearsRestorableEntries();
    void staleComparisonLoadResponseIsIgnored();
    void staleFileDiffResponseIsIgnored();
    void filteredComparisonHidesEntriesUnchangedAgainstCurrentSystem();
    void filteredComparisonMatchesDirectoriesRegardlessOfTrailingSlash();
    void filteredComparisonWithoutRemainingEntriesReportsNoChangesWithoutError();
    void filteredComparisonCurrentComparisonFailureSkipsSecondRequest();
    void staleFilteredComparisonResponseIsIgnored();
    void filteredComparisonPreservesRestoreSourceAndChangeTypes();

    // --- 行指向出力への改行注入に対する多層防御 ---
    void createdParentWithNonCreatedDescendantSuppressesOnlyParent();
    void legitimateCreatedDirectoryTreeStillReachesThePlan();
    void syntheticIntermediateDirectoriesDoNotSuppressCreatedParent();
    void createdParentWithOnlyCreatedDescendantsIsIndistinguishable();

private:
    void prepareStagedRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                              const QString &changeOutput, const QStringList &pathsToCheck, int batchSize,
                              bool flatMode = true);
    static QStringList stagedPathsOf(const FakeRestorePlanTransport &fake);
    static QStringList stagedChangeTypesOf(const FakeRestorePlanTransport &fake);
    void prepareComparisonRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                  int number1, int number2,
                                  const QString &changeOutput, const QStringList &pathsToCheck);
};

void TestFileChangeModel::initTestCase()
{
    // テスト中のQSettings書き込みを一時ディレクトリへ退避させる
    // (開発環境の実設定ファイルを書き換えないため)
    const QString settingsDir = QDir::temp().filePath(QStringLiteral("qsnapper-tst-filechangemodel"));
    QDir().mkpath(settingsDir);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir);
}

void TestFileChangeModel::prepareStagedRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                               const QString &changeOutput, const QStringList &pathsToCheck, int batchSize,
                                               bool flatMode)
{
    model->setConfigName(QStringLiteral("root"));
    model->setSnapshotNumber(42);
    model->setUseDirectRestore(true);
    model->setRestoreBatchSize(batchSize);
    model->setupModelData(changeOutput, flatMode);
    model->setRestorePlanTransportForTesting(fake);
    for (const QString &path : pathsToCheck) {
        model->setItemChecked(path, true);
    }
}

QStringList TestFileChangeModel::stagedPathsOf(const FakeRestorePlanTransport &fake)
{
    QStringList paths;
    for (const FakeRestorePlanTransport::Call &call : fake.calls) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            paths += call.paths;
        }
    }
    return paths;
}

QStringList TestFileChangeModel::stagedChangeTypesOf(const FakeRestorePlanTransport &fake)
{
    QStringList changeTypes;
    for (const FakeRestorePlanTransport::Call &call : fake.calls) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            changeTypes += call.changeTypes;
        }
    }
    return changeTypes;
}

/**
 * @brief 2スナップショット比較モードの復元前提条件を組み立てる
 *
 * loadChangesBetween()は実D-Busサービスを起動してしまうため、
 * 比較条件だけを直接設定して、Pre <--> Post差分ビューと同じ状態を再現する
 */
void TestFileChangeModel::prepareComparisonRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                                   int number1, int number2,
                                                   const QString &changeOutput, const QStringList &pathsToCheck)
{
    model->setConfigName(QStringLiteral("root"));
    model->setUseDirectRestore(true);
    model->setRestoreBatchSize(100);
    model->setComparisonRange(number1, number2, true);
    model->setupModelData(changeOutput, true);
    model->setRestorePlanTransportForTesting(fake);
    for (const QString &path : pathsToCheck) {
        model->setItemChecked(path, true);
    }
}

void TestFileChangeModel::treeBuildRetainsSpacesDeduplicatesAndMarksParents()
{
    TestableFileChangeModel model;
    model.setupModelData(
        QStringLiteral("+.... /home/user/My File.txt\n"
                       "-.... /home/user/My File.txt\n"
                       "c.... /home/user/docs/\n"
                       "m.... /home/user/docs/nested child.txt\n"
                       "t.... /var/cache\n"
                       "+.... /var/cache/item\n"),
        false);

    QCOMPARE(model.rowCount(), 2);
    const QModelIndex homeIndex = model.index(0, 0);
    QCOMPARE(model.data(homeIndex, FileChangeModel::NameRole).toString(), QStringLiteral("home"));
    const QModelIndex userIndex = model.index(0, 0, homeIndex);
    QCOMPARE(model.data(userIndex, FileChangeModel::NameRole).toString(), QStringLiteral("user"));
    QCOMPARE(model.rowCount(userIndex), 2);

    const QModelIndex fileIndex = model.index(0, 0, userIndex);
    QCOMPARE(model.data(fileIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/home/user/My File.txt"));
    QCOMPARE(model.data(fileIndex, FileChangeModel::ChangeTypeRole).toInt(), static_cast<int>(FileChangeItem::Created));

    const QModelIndex docsIndex = model.index(1, 0, userIndex);
    QCOMPARE(model.data(docsIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/home/user/docs/"));
    QVERIFY(model.data(docsIndex, FileChangeModel::IsDirectoryRole).toBool());
    QCOMPARE(model.rowCount(docsIndex), 1);
    QCOMPARE(model.data(model.index(0, 0, docsIndex), FileChangeModel::PathRole).toString(),
             QStringLiteral("/home/user/docs/nested child.txt"));

    const QModelIndex varIndex = model.index(1, 0);
    const QModelIndex cacheIndex = model.index(0, 0, varIndex);
    QCOMPARE(model.data(cacheIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/var/cache/"));
    QVERIFY(model.data(cacheIndex, FileChangeModel::IsDirectoryRole).toBool());
}

void TestFileChangeModel::flatBuildPreservesInputOrderAndExplicitDirectories()
{
    TestableFileChangeModel model;
    model.setupModelData(
        QStringLiteral("m.... /zeta/\n"
                       "+.... /alpha with space\n"
                       "-.... /zeta/\n"
                       "c.... /beta/child\n"),
        true);

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(), QStringLiteral("/zeta/"));
    QVERIFY(model.data(model.index(0, 0), FileChangeModel::IsDirectoryRole).toBool());
    QCOMPARE(model.data(model.index(1, 0), FileChangeModel::PathRole).toString(), QStringLiteral("/alpha with space"));
    QCOMPARE(model.data(model.index(2, 0), FileChangeModel::PathRole).toString(), QStringLiteral("/beta/child"));
}

void TestFileChangeModel::treeBuildHandlesPrefixHeavyPaths()
{
    QString output = QStringLiteral("m.... /prefix\n");
    for (int branch = 0; branch < 1000; ++branch) {
        output += QStringLiteral("+.... /prefix/branch%1/deep/file %1.txt\n").arg(branch, 3, 10, QLatin1Char('0'));
    }

    TestableFileChangeModel model;
    model.setupModelData(output, false);

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex prefixIndex = model.index(0, 0);
    QCOMPARE(model.data(prefixIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/prefix/"));
    QVERIFY(model.data(prefixIndex, FileChangeModel::IsDirectoryRole).toBool());
    QCOMPARE(model.rowCount(prefixIndex), 1000);

    const QModelIndex firstBranchIndex = model.index(0, 0, prefixIndex);
    QCOMPARE(model.data(firstBranchIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/prefix/branch000/"));
    QCOMPARE(model.rowCount(firstBranchIndex), 1);
    const QModelIndex deepIndex = model.index(0, 0, firstBranchIndex);
    QCOMPARE(model.data(model.index(0, 0, deepIndex), FileChangeModel::PathRole).toString(),
             QStringLiteral("/prefix/branch000/deep/file 000.txt"));

    const QModelIndex lastBranchIndex = model.index(999, 0, prefixIndex);
    QCOMPARE(model.data(lastBranchIndex, FileChangeModel::PathRole).toString(), QStringLiteral("/prefix/branch999/"));
}

void TestFileChangeModel::stagedRestoreStagesBoundedChunksAndCommitsOnce()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    QString output;
    QStringList expectedPaths;
    for (int i = 0; i < 10; ++i) {
        const QString path = QStringLiteral("/data/file%1").arg(i);
        output += QStringLiteral("+.... %1\n").arg(path);
        expectedPaths.append(path);
    }
    QStringList expectedTypes;
    for (int i = 0; i < 10; ++i) {
        expectedTypes.append(QStringLiteral("created"));
    }

    prepareStagedRestore(&model, &fake, output, expectedPaths, 3);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());

    // begin完了前はstageされず、各stage完了後にだけ次のstageが発行される
    QCOMPARE(fake.calls.size(), 1);
    fake.completePending(true, fake.nextManifestId);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 0);
    for (int chunk = 0; chunk < 4; ++chunk) {
        fake.completePending(true);
        QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), qMin(chunk + 2, 4));
        if (chunk < 3) {
            QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 0);
        }
        else {
            QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
        }
    }
    fake.completePending(true);

    // beginPlan 1回 + stageEntries ceil(10/3)=4回 + commitPlan 1回
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Begin), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 4);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
    QCOMPARE(fake.calls.size(), 6);

    // beginPlanの引数
    const FakeRestorePlanTransport::Call &beginCall = fake.calls.first();
    QCOMPARE(beginCall.kind, FakeRestorePlanTransport::Call::Begin);
    QCOMPARE(beginCall.configName, QStringLiteral("root"));
    QCOMPARE(beginCall.snapshotNumber, 42);
    // 対カレント比較モードではcounterpartは0 (現在のシステムのsentinel)
    QCOMPARE(beginCall.counterpartNumber, 0);
    QCOMPARE(beginCall.restoreMode, QStringLiteral("direct"));

    // 各チャンクは上限以下で空ではなく、連結結果が元の選択順序と一致する
    QStringList stagedPaths;
    QStringList stagedTypes;
    for (const FakeRestorePlanTransport::Call &call : fake.calls) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            QVERIFY(call.paths.size() <= 3);
            QVERIFY(!call.paths.isEmpty());
            QCOMPARE(call.paths.size(), call.changeTypes.size());
            QCOMPARE(call.manifestId, fake.nextManifestId);
            stagedPaths += call.paths;
            stagedTypes += call.changeTypes;
        }
    }
    QCOMPARE(stagedPaths, expectedPaths);
    QCOMPARE(stagedTypes, expectedTypes);

    // 終端シグナルで完了する
    fake.emitPlanProgress(fake.nextManifestId, 10, 10, QStringLiteral("file09"));
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);
}

void TestFileChangeModel::stagedRestoreSendsNoMutableValuesAfterCommit()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
        QStringLiteral("/data/c"),
        QStringLiteral("/data/d"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 2);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
    const int commitIndex = fake.calls.size() - 1;
    QCOMPARE(fake.calls.at(commitIndex).kind, FakeRestorePlanTransport::Call::Commit);

    // commit後の呼び出しはmanifest idのみを運ぶ (cancelPlan / requestStatusのみ許容)
    for (int i = commitIndex + 1; i < fake.calls.size(); ++i) {
        const FakeRestorePlanTransport::Call &call = fake.calls.at(i);
        QVERIFY(call.kind == FakeRestorePlanTransport::Call::Cancel
                || call.kind == FakeRestorePlanTransport::Call::Status);
        QVERIFY(call.paths.isEmpty());
        QVERIFY(call.changeTypes.isEmpty());
        QVERIFY(call.configName.isEmpty());
        QVERIFY(call.restoreMode.isEmpty());
        QCOMPARE(call.snapshotNumber, 0);
        QCOMPARE(call.counterpartNumber, 0);
        QCOMPARE(call.manifestId, fake.nextManifestId);
    }

    // commit後にキャンセルしてもidのみの呼び出しになる
    model.cancelRestore();
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 1);
    const FakeRestorePlanTransport::Call &cancelCall = fake.calls.last();
    QCOMPARE(cancelCall.kind, FakeRestorePlanTransport::Call::Cancel);
    QVERIFY(cancelCall.paths.isEmpty());
    QVERIFY(cancelCall.changeTypes.isEmpty());
    QVERIFY(cancelCall.configName.isEmpty());
    QVERIFY(cancelCall.restoreMode.isEmpty());
    QCOMPARE(cancelCall.snapshotNumber, 0);
    QCOMPARE(cancelCall.counterpartNumber, 0);
    QCOMPARE(cancelCall.manifestId, fake.nextManifestId);

    fake.completePending(true);
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("cancelled"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
}

void TestFileChangeModel::stagedRestoreProgressIsMonotonicOverOriginalTotal()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    QString output;
    QStringList paths;
    for (int i = 0; i < 10; ++i) {
        const QString path = QStringLiteral("/data/file%1").arg(i);
        output += QStringLiteral("+.... %1\n").arg(path);
        paths.append(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 3);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QSignalSpy progressSpy(&model, &FileChangeModel::restoreProgress);
    fake.emitPlanProgress(fake.nextManifestId, 5, 10, QStringLiteral("file05"));
    fake.emitPlanProgress(fake.nextManifestId, 3, 10, QStringLiteral("file03"));
    fake.emitPlanProgress(fake.nextManifestId, 10, 10, QStringLiteral("file09"));

    QCOMPARE(progressSpy.count(), 3);
    int previousCurrent = -1;
    for (int i = 0; i < progressSpy.count(); ++i) {
        const int current = progressSpy.at(i).at(0).toInt();
        const int total = progressSpy.at(i).at(1).toInt();
        QVERIFY(current >= previousCurrent);
        // totalは常にmanifest全体の総エントリ数 (元の選択数)
        QCOMPARE(total, 10);
        previousCurrent = current;
    }
    QCOMPARE(progressSpy.at(0).at(0).toInt(), 5);
    QCOMPARE(progressSpy.at(1).at(0).toInt(), 5);
    QCOMPARE(progressSpy.at(2).at(0).toInt(), 10);
}

void TestFileChangeModel::stagedRestoreCompletedTerminalEmitsCompletionOnce()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);

    // 状態はリセット済みのため、重複した終端シグナルは無視される
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
}

void TestFileChangeModel::stagedRestoreServiceVanishAfterCommitFinishesWithFailure()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);

    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QCOMPARE(completedSpy.count(), 0);
    const int cancelCount = fake.countKind(FakeRestorePlanTransport::Call::Cancel);
    const int unsubscribeCount = fake.unsubscribeCount;
    fake.emitServiceVanished();

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
    QVERIFY(errorSpy.count() >= 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), cancelCount);
    QVERIFY(fake.unsubscribeCount > unsubscribeCount);

    // 状態はリセット済みのため、遅延した終端シグナルは無視される
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
}

void TestFileChangeModel::stagedRestoreServiceVanishDuringStagingFinishesWithFailure()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);

    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());
    fake.completePending(true, fake.nextManifestId);
    fake.emitServiceVanished();

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
    QVERIFY(errorSpy.count() >= 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 0);
}

void TestFileChangeModel::stagedRestoreStageFailureAbortsFlowWithError()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    QString output;
    QStringList paths;
    for (int i = 0; i < 10; ++i) {
        const QString path = QStringLiteral("/data/file%1").arg(i);
        output += QStringLiteral("+.... %1\n").arg(path);
        paths.append(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 3);

    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());

    fake.completePending(true, fake.nextManifestId);                           // beginPlan成功
    fake.completePending(true);                                                // チャンク0成功
    fake.completePending(false, QString(), QStringLiteral("chunk rejected"));  // チャンク1失敗

    // 以降のstageEntriesもcommitPlanも発行されない
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 2);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 0);
    // ベストエフォートの計画破棄のみ発行される
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 1);
    QCOMPARE(fake.calls.last().manifestId, fake.nextManifestId);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);

    // cancelコールバックを完了しても継続の呼び出しは発生しない
    fake.completePending(true);
    QCOMPARE(fake.calls.size(), 4);
}

void TestFileChangeModel::stagedRestoreCommitFailureAbortsFlowWithError()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
        QStringLiteral("/data/c"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);

    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());

    fake.completePending(true, fake.nextManifestId);                            // beginPlan成功
    fake.completePending(true);                                                 // チャンク0成功
    fake.completePending(false, QString(), QStringLiteral("commit rejected"));  // commit失敗

    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 1);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);

    // cancelコールバックを完了しても継続の呼び出しは発生しない
    fake.completePending(true);
    QCOMPARE(fake.calls.size(), 4);
}

void TestFileChangeModel::stagedRestoreCancelWaitsForTerminalCancelled()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    model.cancelRestore();

    // cancelPlanは記録されるが、完了はまだ通知されない
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 1);
    QCOMPARE(completedSpy.count(), 0);

    fake.completePending(true);
    QCOMPARE(completedSpy.count(), 0);

    // 終端シグナル (cancelled) で初めて完了が通知される
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("cancelled"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
}

void TestFileChangeModel::stagedRestoreIgnoresForeignManifestId()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList paths = {
        QStringLiteral("/data/a"),
        QStringLiteral("/data/b"),
    };
    QString output;
    for (const QString &path : paths) {
        output += QStringLiteral("+.... %1\n").arg(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 100);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QSignalSpy progressSpy(&model, &FileChangeModel::restoreProgress);
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    // 異なるmanifest idのシグナルは全て無視される
    fake.emitPlanProgress(QStringLiteral("other-manifest"), 1, 10, QStringLiteral("x"));
    fake.emitPlanFinished(QStringLiteral("other-manifest"), QStringLiteral("completed"), QString());
    QCOMPARE(progressSpy.count(), 0);
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    // 実行中の計画のidは引き続き受け付けられる
    fake.emitPlanProgress(fake.nextManifestId, 2, 2, QStringLiteral("y"));
    QCOMPARE(progressSpy.count(), 1);
    QCOMPARE(progressSpy.at(0).at(0).toInt(), 2);
    QCOMPARE(progressSpy.at(0).at(1).toInt(), 2);
}

void TestFileChangeModel::stagedRestoreNormalizesTrailingSlashDirectory()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    // フラットモードではディレクトリ行は末尾スラッシュ付きで保持される
    const QString output = QStringLiteral("c.... /zeta/\n"
                                           "+.... /zeta/child.txt\n"
                                           "m.... /omega\n");
    const QStringList pathsToCheck = {
        QStringLiteral("/zeta/"),
        QStringLiteral("/zeta/child.txt"),
        QStringLiteral("/omega"),
    };

    prepareStagedRestore(&model, &fake, output, pathsToCheck, 100);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    // 末尾スラッシュは正規化されても破棄されず、commitまで到達する
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
    QStringList stagedPaths;
    QStringList stagedTypes;
    for (const FakeRestorePlanTransport::Call &call : fake.calls) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            stagedPaths += call.paths;
            stagedTypes += call.changeTypes;
        }
    }
    QCOMPARE(stagedPaths, QStringList({
        QStringLiteral("/zeta"),
        QStringLiteral("/zeta/child.txt"),
        QStringLiteral("/omega"),
    }));
    // 'c' (content modified) はModified、'+' はCreatedとして送出される
    QCOMPARE(stagedTypes, QStringList({
        QStringLiteral("modified"),
        QStringLiteral("created"),
        QStringLiteral("modified"),
    }));

    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);
}

/**
 * @brief サーバ側で拒否される復元エントリをstaging前に除外する
 */
void TestFileChangeModel::stagedRestoreFiltersInvalidEntriesBeforeStaging()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QString controlPath = QStringLiteral("/bad") + QChar(0x1f) + QStringLiteral("name");
    const QString output = QStringLiteral("+.... /valid\n"
                                           "+.... /bad/../path\n"
                                           "+.... /.snapshots/hidden\n")
        + QStringLiteral("+.... ") + controlPath + QLatin1Char('\n');
    const QStringList pathsToCheck = {
        QStringLiteral("/valid"),
        QStringLiteral("/bad/../path"),
        QStringLiteral("/.snapshots/hidden"),
        controlPath,
    };

    prepareStagedRestore(&model, &fake, output, pathsToCheck, 100);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);
    const FakeRestorePlanTransport::Call &stageCall = fake.calls.at(1);
    QCOMPARE(stageCall.paths, QStringList({QStringLiteral("/valid")}));
    QCOMPARE(stageCall.changeTypes, QStringList({QStringLiteral("created")}));

    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);
}

void TestFileChangeModel::stagedRestoreCancelDuringStagingFinishesLocally()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    QString output;
    QStringList paths;
    for (int i = 0; i < 10; ++i) {
        const QString path = QStringLiteral("/data/file%1").arg(i);
        output += QStringLiteral("+.... %1\n").arg(path);
        paths.append(path);
    }

    prepareStagedRestore(&model, &fake, output, paths, 3);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QVERIFY(model.restoreCheckedItems());

    fake.completePending(true, fake.nextManifestId);  // beginPlan成功
    fake.completePending(true);                       // チャンク0成功 (チャンク1が発行済み)

    // staging中 (commit前) にキャンセルするとローカルで完了扱いになる
    model.cancelRestore();
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Cancel), 1);
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);

    // 待機中のチャンクコールバックを完了しても次のチャンクは送出されない
    fake.completePending(true);  // チャンク1のコールバック (中断済みのため無視される)
    fake.completePending(true);  // cancelPlanのコールバック
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 2);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 0);
    QCOMPARE(fake.calls.size(), 4);
    QCOMPARE(completedSpy.count(), 1);
}

/**
 * @brief Pre <--> Post差分からの復元が比較元スナップショットを使い、比較モードを保つ
 */
void TestFileChangeModel::prePostRestoreUsesComparisonSourceAndPreservesBetweenMode()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    // Pre #100 --> Post #101の比較結果
    // createdはPostにだけ存在する = Preには無いため、Preへの復元ではlive側から削除される
    const QString output = QStringLiteral("+.... /etc/installed.conf\n"
                                          "-.... /etc/removed.conf\n"
                                          "c.... /etc/changed.conf\n");
    const QStringList pathsToCheck = {
        QStringLiteral("/etc/installed.conf"),
        QStringLiteral("/etc/removed.conf"),
        QStringLiteral("/etc/changed.conf"),
    };

    prepareComparisonRestore(&model, &fake, 100, 101, output, pathsToCheck);

    QVERIFY(model.restoreCheckedItemsFrom(100));

    // 復元元は比較の第1オペランド (Pre #100)
    // 比較先 (Post #101) であってはならない
    const FakeRestorePlanTransport::Call &beginCall = fake.calls.first();
    QCOMPARE(beginCall.kind, FakeRestorePlanTransport::Call::Begin);
    QCOMPARE(beginCall.configName, QStringLiteral("root"));
    QCOMPARE(beginCall.snapshotNumber, 100);

    fake.completeAllPendingOk();

    // changeTypeはクライアント側で反転させず、比較結果のまま送出する
    const FakeRestorePlanTransport::Call &stageCall = fake.calls.at(1);
    QCOMPARE(stageCall.paths, pathsToCheck);
    QCOMPARE(stageCall.changeTypes, QStringList({QStringLiteral("created"),
                                                 QStringLiteral("deleted"),
                                                 QStringLiteral("modified")}));

    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());

    // 復元しても比較モードは維持されるため、引数なしの復元も比較元 (Pre #100) を使用する
    // (snapshotNumberプロパティは比較先#101を保持しているので、リセットが起きれば#101になる)
    fake.calls.clear();
    for (const QString &path : pathsToCheck) {
        model.setItemChecked(path, true);
    }
    QVERIFY(model.restoreCheckedItems());
    QCOMPARE(fake.calls.first().snapshotNumber, 100);
}

/**
 * @brief 比較先スナップショットを復元元に指定した復元が拒否される
 */
void TestFileChangeModel::prePostRestoreFromComparisonTargetIsRejected()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList pathsToCheck = {QStringLiteral("/etc/installed.conf")};

    prepareComparisonRestore(&model, &fake, 100, 101,
                             QStringLiteral("+.... /etc/installed.conf\n"), pathsToCheck);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    // Post #101を復元元にするとcreated/deletedの意味が反転し、残すべきファイルを削除してしまう
    QVERIFY(!model.restoreCheckedItemsFrom(101));
    QVERIFY(fake.calls.isEmpty());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
}

/**
 * @brief beginPlanに比較相手 (counterpart) が両方向で正しく渡されることを検証する
 *
 * counterpartは表示中のエントリを生んだ比較条件 (m_loadedComparison.number2) であり、
 * サーバはこの値で権威ある比較を再構築して凍結済みentryを検証する
 * 復元元と同じ向きでなければCREATEDとDELETEDが反転するため、
 * Pre --> PostとPost --> Preの両方向で意図どおりの値が届くことを固定する
 */
void TestFileChangeModel::beginPlanCarriesCounterpartFromLoadedComparisonInBothDirections()
{
    const QString output = QStringLiteral("+.... /etc/installed.conf\n"
                                          "-.... /etc/removed.conf\n");

    // --- Pre #100 --> Post #101 (Revert to Pre) ---
    {
        FakeRestorePlanTransport fake;
        TestableFileChangeModel model;
        const QStringList pathsToCheck = {QStringLiteral("/etc/installed.conf")};
        prepareComparisonRestore(&model, &fake, 100, 101, output, pathsToCheck);

        QVERIFY(model.restoreCheckedItemsFrom(100));

        const FakeRestorePlanTransport::Call &beginCall = fake.calls.first();
        QCOMPARE(beginCall.kind, FakeRestorePlanTransport::Call::Begin);
        QCOMPARE(beginCall.snapshotNumber, 100);
        QCOMPARE(beginCall.counterpartNumber, 101);
    }

    // --- Post #101 --> Pre #100 (Re-apply to Post) ---
    {
        FakeRestorePlanTransport fake;
        TestableFileChangeModel model;
        const QStringList pathsToCheck = {QStringLiteral("/etc/installed.conf")};
        prepareComparisonRestore(&model, &fake, 101, 100, output, pathsToCheck);

        QVERIFY(model.restoreCheckedItemsFrom(101));

        const FakeRestorePlanTransport::Call &beginCall = fake.calls.first();
        QCOMPARE(beginCall.kind, FakeRestorePlanTransport::Call::Begin);
        QCOMPARE(beginCall.snapshotNumber, 101);
        QCOMPARE(beginCall.counterpartNumber, 100);
    }
}

/**
 * @brief 対カレント比較モードではbeginPlanにcounterpart 0が渡されることを検証する
 *
 * 0は「現在のシステム」を表すsentinelである
 * snapshotNumberプロパティ (比較相手ではない) を誤って渡すと、
 * サーバ側で別snapshotとの比較が再構築され、検証対象が実行対象と食い違う
 */
void TestFileChangeModel::beginPlanCarriesZeroCounterpartInVsCurrentMode()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList pathsToCheck = {QStringLiteral("/data/file0")};

    prepareStagedRestore(&model, &fake, QStringLiteral("+.... /data/file0\n"),
                         pathsToCheck, 100);

    QVERIFY(model.restoreCheckedItemsFrom(42));

    const FakeRestorePlanTransport::Call &beginCall = fake.calls.first();
    QCOMPARE(beginCall.kind, FakeRestorePlanTransport::Call::Begin);
    QCOMPARE(beginCall.snapshotNumber, 42);
    QCOMPARE(beginCall.counterpartNumber, 0);
}

/**
 * @brief 空のstatusFlagsを持つ合成親ディレクトリが計画へ載らないことを検証する
 *
 * ツリー構築で合成された中間ディレクトリは比較結果に由来せず、
 * statusFlagsが空でchangeTypeがModifiedになる
 * stagerはchangeTypeがModifiedの親に実レコードの子が居る場合、親を計画へ載せない
 * 合成ノードが誤ってStageRestoreEntriesへ届くと、
 * 権威ある比較に存在しないパスとしてcommit時の検証が計画全体をrejectしてしまう
 */
void TestFileChangeModel::syntheticParentWithEmptyStatusFlagsNeverReachesThePlan()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    // /etc/foo/barは出力に現れず、ツリー構築時に合成される
    const QString output = QStringLiteral("+.... /etc/foo\n"
                                          "+.... /etc/foo/bar/baz.conf\n");
    prepareStagedRestore(&model, &fake, output,
                         {QStringLiteral("/etc/foo/")}, 100, /*flatMode=*/false);

    // 合成ノードのstatusFlagsは空であることを直接観測する
    // (getItem / findItemIndexはprivateのため、公開APIで木を走査する)
    const std::function<QModelIndex(const QString &)> findPath =
        [&](const QString &path) -> QModelIndex {
            const std::function<QModelIndex(const QModelIndex &)> walk =
                [&](const QModelIndex &parent) -> QModelIndex {
                    for (int row = 0; row < model.rowCount(parent); ++row) {
                        const QModelIndex index = model.index(row, 0, parent);
                        if (model.data(index, FileChangeModel::PathRole).toString() == path) {
                            return index;
                        }
                        const QModelIndex child = walk(index);
                        if (child.isValid()) {
                            return child;
                        }
                    }
                    return QModelIndex();
                };
            return walk(QModelIndex());
        };
    const QModelIndex fooIndex = findPath(QStringLiteral("/etc/foo/"));
    QVERIFY(fooIndex.isValid());
    const QModelIndex barIndex = findPath(QStringLiteral("/etc/foo/bar/"));
    QVERIFY(barIndex.isValid());
    QCOMPARE(model.data(barIndex, FileChangeModel::StatusFlagsRole).toString(), QString());

    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    // 合成ノード (/etc/foo/bar) は計画に載らない
    const QStringList stagedPaths = stagedPathsOf(fake);
    QVERIFY(stagedPaths.contains(QStringLiteral("/etc/foo")));
    QVERIFY(!stagedPaths.contains(QStringLiteral("/etc/foo/bar")));
    QVERIFY(stagedPaths.contains(QStringLiteral("/etc/foo/bar/baz.conf")));
}

/**
 * @brief 対カレント比較では指定済みスナップショット以外からの復元が拒否される
 */
void TestFileChangeModel::restoreFromMismatchedSnapshotIsRejectedInVsCurrentMode()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    const QStringList pathsToCheck = {QStringLiteral("/data/file0")};

    prepareStagedRestore(&model, &fake, QStringLiteral("+.... /data/file0\n"), pathsToCheck, 100);

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);

    QVERIFY(!model.restoreCheckedItemsFrom(7));
    QVERIFY(fake.calls.isEmpty());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);

    // 一致する番号なら従来どおり復元できる (Singleスナップショットの挙動は不変)
    QVERIFY(model.restoreCheckedItemsFrom(42));
    QCOMPARE(fake.calls.first().snapshotNumber, 42);
}

/**
 * @brief 単一ファイル復元でも復元元の不整合が拒否される
 */
void TestFileChangeModel::singleFileRestoreFromMismatchedSnapshotIsRejected()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    prepareComparisonRestore(&model, &fake, 100, 101,
                             QStringLiteral("+.... /etc/installed.conf\n"), QStringList());

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    // 不整合はD-Bus接続より前に遮断されるため、テスト実行が実サービスを起動することはない
    model.restoreSingleFileFrom(QStringLiteral("/etc/installed.conf"), 101);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
}

void TestFileChangeModel::failedComparisonReplacementClearsRestorableEntries()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setRestorePlanTransportForTesting(&fake);

    model.loadChangesBetween(100, 101, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);
    model.completeStringReply(0, true, QStringLiteral("+.... /etc/installed.conf\n"));
    QCOMPARE(model.rowCount(), 1);

    model.setItemChecked(QStringLiteral("/etc/installed.conf"), true);
    model.loadChangesBetween(101, 100, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);
    model.completeStringReply(0, false, QString(), QStringLiteral("replacement failed"));

    QVERIFY(!model.restoreCheckedItemsFrom(101));
    QVERIFY(fake.calls.isEmpty());
    QCOMPARE(model.rowCount(), 0);
}

void TestFileChangeModel::staleComparisonLoadResponseIsIgnored()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setRestorePlanTransportForTesting(&fake);

    model.loadChangesBetween(100, 101, true);
    model.loadChangesBetween(101, 100, true);
    QCOMPARE(model.pendingStringReplies.size(), 2);
    QCOMPARE(model.pendingStringReplies.at(1).methodName, QStringLiteral("GetFileChangesBetween"));
    QCOMPARE(model.pendingStringReplies.at(1).arguments.at(1).toInt(), 101);
    QCOMPARE(model.pendingStringReplies.at(1).arguments.at(2).toInt(), 100);

    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    model.completeStringReply(1, true, QStringLiteral("-.... /etc/post-only.conf\n"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/post-only.conf"));

    model.completeStringReply(0, false, QString(), QStringLiteral("old request failed"));
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/post-only.conf"));

    model.setItemChecked(QStringLiteral("/etc/post-only.conf"), true);
    QVERIFY(model.restoreCheckedItemsFrom(101));
    QCOMPARE(fake.calls.first().snapshotNumber, 101);
}

void TestFileChangeModel::staleFileDiffResponseIsIgnored()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setComparisonRange(100, 101, true);
    model.getFileDiffAndDetails(QStringLiteral("/etc/changed.conf"));
    QCOMPARE(model.pendingStringReplies.size(), 1);

    model.setComparisonRange(101, 100, true);
    model.getFileDiffAndDetails(QStringLiteral("/etc/changed.conf"));
    QCOMPARE(model.pendingStringReplies.size(), 2);

    QSignalSpy diffSpy(&model, &FileChangeModel::fileDiffAndDetailsReady);
    model.completeStringReply(1, true,
                              QStringLiteral("snapshot=post\n---DIFF_SEPARATOR---\npost diff"));
    QCOMPARE(diffSpy.count(), 1);
    QCOMPARE(diffSpy.at(0).at(1).toMap().value(QStringLiteral("snapshot")).toString(),
             QStringLiteral("post"));
    QCOMPARE(diffSpy.at(0).at(2).toString(), QStringLiteral("post diff"));

    model.completeStringReply(0, true,
                              QStringLiteral("snapshot=pre\n---DIFF_SEPARATOR---\npre diff"));
    QCOMPARE(diffSpy.count(), 1);
}

/**
 * @brief 復元元と現在のシステムの間に差異が無いエントリが表示対象から除外される
 */
void TestFileChangeModel::filteredComparisonHidesEntriesUnchangedAgainstCurrentSystem()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));

    model.loadChangesBetween(100, 101, true, true);

    // 2つの比較は直列に要求する
    // 並行させるとPolkit認可のキャッシュ前に2件の認可要求が飛び、認証プロンプトが二重に出る可能性があるため
    QCOMPARE(model.pendingStringReplies.size(), 1);
    QCOMPARE(model.pendingStringReplies.at(0).methodName, QStringLiteral("GetFileChanges"));
    QCOMPARE(model.pendingStringReplies.at(0).arguments.at(1).toInt(), 100);

    model.completeStringReply(0, true,
                              QStringLiteral("+.... /usr/bin/newtool\n"
                                             "c.... /etc/ld.so.cache\n"
                                             "-.... /etc/removed.conf\n"
                                             "+.... /etc/manually-added.conf\n"));

    // 対カレント比較を先に完了させることで、
    // サービス側の単一スロットComparisonキャッシュには「比較元 --> 比較先」が最後に残り、後続のファイル差分取得が再利用できる
    QCOMPARE(model.pendingStringReplies.size(), 1);
    QCOMPARE(model.pendingStringReplies.at(0).methodName, QStringLiteral("GetFileChangesBetween"));
    QCOMPARE(model.pendingStringReplies.at(0).arguments.at(1).toInt(), 100);
    QCOMPARE(model.pendingStringReplies.at(0).arguments.at(2).toInt(), 101);
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.isLoading());

    model.completeStringReply(0, true,
                              QStringLiteral("+.... /usr/bin/newtool\n"
                                             "c.... /etc/ld.so.cache\n"
                                             "-.... /etc/removed.conf\n"
                                             "c.... /etc/already-restored.conf\n"));

    QVERIFY(!model.isLoading());
    QVERIFY(model.hasChanges());

    // 既に復元済みの/etc/already-restored.confは消え、
    // Pre <--> Postの範囲外にある/etc/manually-added.confは追加されない
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/usr/bin/newtool"));
    QCOMPARE(model.data(model.index(1, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/ld.so.cache"));
    QCOMPARE(model.data(model.index(2, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/removed.conf"));

    // 変更種別は「比較元 --> 比較先」のまま保持される
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::ChangeTypeRole).toInt(),
             static_cast<int>(FileChangeItem::Created));
    QCOMPARE(model.data(model.index(2, 0), FileChangeModel::ChangeTypeRole).toInt(),
             static_cast<int>(FileChangeItem::Deleted));
}

/**
 * @brief 末尾スラッシュ表記の揺れがあっても同一パスとして突き合わせる
 */
void TestFileChangeModel::filteredComparisonMatchesDirectoriesRegardlessOfTrailingSlash()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));

    model.loadChangesBetween(100, 101, true, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);

    model.completeStringReply(0, true,
                              QStringLiteral("c.... /etc/conf.d\n"
                                             "c.... /etc/other.conf\n"));
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.isLoading());

    // 同じディレクトリが末尾スラッシュ付きで報告されても突き合わせに成功する
    model.completeStringReply(0, true,
                              QStringLiteral("c.... /etc/conf.d/\n"
                                             "c.... /etc/unchanged.conf\n"));

    QVERIFY(!model.isLoading());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/conf.d/"));
}

/**
 * @brief 全エントリが除外された場合、エラーではなく「差分なし」として扱う
 */
void TestFileChangeModel::filteredComparisonWithoutRemainingEntriesReportsNoChangesWithoutError()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    model.loadChangesBetween(100, 101, true, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);

    // 現在のシステムは既に復元元と一致しているため、対カレント比較は空を返す
    model.completeStringReply(0, true, QString());
    model.completeStringReply(0, true, QStringLiteral("c.... /etc/already-restored.conf\n"));

    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(!model.isLoading());
    QVERIFY(!model.hasChanges());
    QCOMPARE(model.rowCount(), 0);
}

/**
 * @brief 対カレント比較が失敗した場合、2スナップショット比較を要求せず一覧も復活させない
 */
void TestFileChangeModel::filteredComparisonCurrentComparisonFailureSkipsSecondRequest()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setRestorePlanTransportForTesting(&fake);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    model.loadChangesBetween(100, 101, true, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);

    model.completeStringReply(0, false, QString(), QStringLiteral("current comparison failed"));

    // フィルタ集合が得られない以上、2スナップショット比較の結果だけで一覧を作ってはならない
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(model.pendingStringReplies.isEmpty());
    QVERIFY(!model.isLoading());
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.hasChanges());

    model.setItemChecked(QStringLiteral("/etc/installed.conf"), true);
    QVERIFY(!model.restoreCheckedItemsFrom(100));
    QVERIFY(fake.calls.isEmpty());
}

/**
 * @brief 比較条件の切替後に届いた古いフィルタ応答がモデルへ公開されない
 */
void TestFileChangeModel::staleFilteredComparisonResponseIsIgnored()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));

    model.loadChangesBetween(100, 101, true, true);
    model.loadChangesBetween(101, 100, true, true);
    QCOMPARE(model.pendingStringReplies.size(), 2);

    // 新しい要求 (index 1) を完了させ、その後続要求も完了させる
    model.completeStringReply(1, true, QStringLiteral("-.... /etc/post-only.conf\n"));
    QCOMPARE(model.pendingStringReplies.size(), 2);
    QCOMPARE(model.pendingStringReplies.at(1).methodName, QStringLiteral("GetFileChangesBetween"));
    model.completeStringReply(1, true, QStringLiteral("-.... /etc/post-only.conf\n"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/post-only.conf"));

    // 古い要求 (index 0) の応答は世代が一致しないため破棄され、後続要求も発行されない
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);
    model.completeStringReply(0, true, QStringLiteral("+.... /etc/stale.conf\n"));

    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(model.pendingStringReplies.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), FileChangeModel::PathRole).toString(),
             QStringLiteral("/etc/post-only.conf"));
}

/**
 * @brief 絞り込み後も復元元は比較の第1オペランドのままで、変更種別も書き換わらない
 */
void TestFileChangeModel::filteredComparisonPreservesRestoreSourceAndChangeTypes()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setUseDirectRestore(true);
    model.setRestoreBatchSize(100);
    model.setRestorePlanTransportForTesting(&fake);

    model.loadChangesBetween(100, 101, true, true);
    QCOMPARE(model.pendingStringReplies.size(), 1);
    model.completeStringReply(0, true,
                              QStringLiteral("+.... /etc/installed.conf\n"
                                             "-.... /etc/removed.conf\n"));
    model.completeStringReply(0, true,
                              QStringLiteral("+.... /etc/installed.conf\n"
                                             "-.... /etc/removed.conf\n"
                                             "c.... /etc/already-restored.conf\n"));

    QCOMPARE(model.rowCount(), 2);
    model.setItemChecked(QStringLiteral("/etc/installed.conf"), true);
    model.setItemChecked(QStringLiteral("/etc/removed.conf"), true);

    // 絞り込みで除外済みのパスは、明示的にチェックしてもモデルに存在しないため送出されない
    model.setItemChecked(QStringLiteral("/etc/already-restored.conf"), true);

    QVERIFY(model.restoreCheckedItemsFrom(100));
    QCOMPARE(fake.calls.first().snapshotNumber, 100);

    fake.completeAllPendingOk();

    const FakeRestorePlanTransport::Call &stageCall = fake.calls.at(1);
    QCOMPARE(stageCall.paths, QStringList({QStringLiteral("/etc/installed.conf"),
                                           QStringLiteral("/etc/removed.conf")}));
    QCOMPARE(stageCall.changeTypes, QStringList({QStringLiteral("created"),
                                                 QStringLiteral("deleted")}));
}

/**
 * @brief 矛盾するcreated親ディレクトリを復元計画から落とし、配下の本物は残すことを検証する
 *
 * 攻撃者はパスに改行を混入させることで、1エントリを複数行へ割り、"+.... /etc"のような偽の親ディレクトリ行を注入できる
 * createdディレクトリはlive側の再帰削除になるため、そのまま計画へ載ると配下の非選択パスごと削除され得る
 * 復元元に存在しないディレクトリの配下は必ずcreatedになるという不変条件により、非createdの実レコードを含む場合は親自身だけを落とす
 */
void TestFileChangeModel::createdParentWithNonCreatedDescendantSuppressesOnlyParent()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    // "+.... /etc"は、carrierディレクトリ名に埋め込まれた改行によって注入された偽の行
    const QString output = QStringLiteral("+.... /etc\n"
                                          "c.... /etc/existing.conf\n"
                                          "+.... /etc/added.conf\n");
    prepareStagedRestore(&model, &fake, output,
                         {QStringLiteral("/etc/")}, 100, /*flatMode=*/false);

    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    const QStringList stagedPaths = stagedPathsOf(fake);

    // 偽の親は計画に載らない
    QVERIFY(!stagedPaths.contains(QStringLiteral("/etc")));

    // 配下の本物は従来どおり載る (過剰に落とさない)
    QCOMPARE(stagedPaths, QStringList({
        QStringLiteral("/etc/added.conf"),
        QStringLiteral("/etc/existing.conf"),
    }));
    QCOMPARE(stagedChangeTypesOf(fake), QStringList({
        QStringLiteral("created"),
        QStringLiteral("modified"),
    }));
}

/**
 * @brief 正規のcreatedディレクトリ木は従来どおり計画に載ることを検証する
 *
 * README.mdに明記された「Preへ戻すと新規ディレクトリを再帰削除する」挙動を壊さないこと
 */
void TestFileChangeModel::legitimateCreatedDirectoryTreeStillReachesThePlan()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    // パッケージインストールで"/etc/foo/"が丸ごと新設された場合の典型的な出力
    const QString output = QStringLiteral("+.... /etc/foo\n"
                                          "+.... /etc/foo/a.conf\n"
                                          "+.... /etc/foo/b.conf\n");
    prepareStagedRestore(&model, &fake, output,
                         {QStringLiteral("/etc/foo/")}, 100, /*flatMode=*/false);

    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QCOMPARE(stagedPathsOf(fake), QStringList({
        QStringLiteral("/etc/foo"),
        QStringLiteral("/etc/foo/a.conf"),
        QStringLiteral("/etc/foo/b.conf"),
    }));
    QCOMPARE(stagedChangeTypesOf(fake), QStringList({
        QStringLiteral("created"),
        QStringLiteral("created"),
        QStringLiteral("created"),
    }));
}

/**
 * @brief ツリー構築で合成された中間ディレクトリが誤検知を起こさないことを検証する
 *
 * 合成ノードはstatusFlagsが空でchangeTypeがModifiedになるが、比較結果に由来しないため矛盾の証拠にはならない
 * Pre/Post表示フィルタが実レコードを間引いた場合にも生じる
 */
void TestFileChangeModel::syntheticIntermediateDirectoriesDoNotSuppressCreatedParent()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    // /etc/foo/barは出力に現れず、ツリー構築時に合成される
    const QString output = QStringLiteral("+.... /etc/foo\n"
                                          "+.... /etc/foo/bar/baz.conf\n");
    prepareStagedRestore(&model, &fake, output,
                         {QStringLiteral("/etc/foo/")}, 100, /*flatMode=*/false);

    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    const QStringList stagedPaths = stagedPathsOf(fake);

    // 合成された/etc/foo/barは実レコードではないため計画に載らず、かつ/etc/fooの抑止理由にもならない
    QVERIFY(stagedPaths.contains(QStringLiteral("/etc/foo")));
    QVERIFY(!stagedPaths.contains(QStringLiteral("/etc/foo/bar")));
    QVERIFY(stagedPaths.contains(QStringLiteral("/etc/foo/bar/baz.conf")));
}

/**
 * @brief 配下が全てcreatedの偽親は、クライアントからは識別できないことを記録する
 *
 * 行形式だけを受け取るクライアントにとって、この入力は「新規ファイルだけが追加された正規の新設ディレクトリ」と完全に同一である
 * 由来を示す情報がレコードに存在しないため、本ケースを止められるのはクライアント側の判定ではなく、
 * サーバ側のシリアライズfail-closedと、created削除前の復元元存在確認である
 *
 * hasOnlyCreatedRecordsBeneath()をここまで拡張しようとすると正規の出力を拒否してしまうため、本テストは現在の挙動を仕様として固定する
 */
void TestFileChangeModel::createdParentWithOnlyCreatedDescendantsIsIndistinguishable()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    const QString output = QStringLiteral("+.... /etc\n"
                                          "+.... /etc/added.conf\n");
    prepareStagedRestore(&model, &fake, output,
                         {QStringLiteral("/etc/")}, 100, /*flatMode=*/false);

    QVERIFY(model.restoreCheckedItems());
    fake.completeAllPendingOk();

    QCOMPARE(stagedPathsOf(fake), QStringList({
        QStringLiteral("/etc"),
        QStringLiteral("/etc/added.conf"),
    }));
}

QTEST_APPLESS_MAIN(TestFileChangeModel)
#include "tst_filechangemodel.moc"
