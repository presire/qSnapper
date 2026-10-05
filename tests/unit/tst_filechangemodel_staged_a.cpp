/**
 * @file tst_filechangemodel_staged_a.cpp
 * @brief staged restore計画テストの前半
 *
 * チャンク分割と確定と進捗と端末状態の基本動作を検証する
 */

#include "tst_filechangemodel.h"
#include <QtTest>
#include <QSignalSpy>
#include <QFileInfo>


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
    fake.emitPlanProgress(fake.nextManifestId, 10, 10);
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
    fake.emitPlanProgress(fake.nextManifestId, 5, 10);
    fake.emitPlanProgress(fake.nextManifestId, 3, 10);
    fake.emitPlanProgress(fake.nextManifestId, 10, 10);

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

    // サーバのsignalはpathを運ばないため、ファイル名はstageした順序から求める
    QStringList stagedPaths;
    for (const FakeRestorePlanTransport::Call &call : fake.calls) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            stagedPaths += call.paths;
        }
    }
    QCOMPARE(stagedPaths.size(), 10);
    QCOMPARE(progressSpy.at(0).at(2).toString(), QFileInfo(stagedPaths.at(4)).fileName());
    QCOMPARE(progressSpy.at(1).at(2).toString(), QFileInfo(stagedPaths.at(4)).fileName());
    QCOMPARE(progressSpy.at(2).at(2).toString(), QFileInfo(stagedPaths.at(9)).fileName());
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

/**
 * @brief 失敗終端でownerにだけ詳細を問い合わせることを確認する
 */
void TestFileChangeModel::stagedRestoreFailedTerminalRequestsOwnerStatus()
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

    // 終端signalは固定文言だけを運ぶため、失敗の詳細はowner束縛のGetRestorePlanStatusで取得する
    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);
    const int statusCount = fake.countKind(FakeRestorePlanTransport::Call::Status);
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("failed"),
                          QStringLiteral("Restore failed"));

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), false);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Status), statusCount + 1);
    QCOMPARE(fake.calls.last().kind, FakeRestorePlanTransport::Call::Status);
    QCOMPARE(fake.calls.last().manifestId, fake.nextManifestId);

    // 状態のリセット後に応答が届いても、モデルの状態には影響しない
    fake.completeAllPendingOk();
    QCOMPARE(completedSpy.count(), 1);
}
