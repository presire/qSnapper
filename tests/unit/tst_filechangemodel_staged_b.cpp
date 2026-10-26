/**
 * @file tst_filechangemodel_staged_b.cpp
 * @brief staged restore計画テストの後半
 *
 * 消失と失敗とキャンセルと検証除外の振る舞いを検証する
 */

#include "tst_filechangemodel.h"


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
    fake.emitPlanProgress(QStringLiteral("other-manifest"), 1, 10);
    fake.emitPlanFinished(QStringLiteral("other-manifest"), QStringLiteral("completed"), QString());
    QCOMPARE(progressSpy.count(), 0);
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    // 実行中の計画のidは引き続き受け付けられる
    fake.emitPlanProgress(fake.nextManifestId, 2, 2);
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
