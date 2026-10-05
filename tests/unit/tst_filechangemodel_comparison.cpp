/**
 * @file tst_filechangemodel_comparison.cpp
 * @brief 比較条件と復元元検証のテスト
 *
 * PreとPostの向きと復元元の一致検証と古い応答の破棄を検証する
 */

#include "tst_filechangemodel.h"
#include <QtTest>
#include <QSignalSpy>
#include <QFileInfo>


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

/**
 * @brief 単一ファイル復元もstaged restore (Begin / Stage / Commit) で実行される
 *
 * 廃止したlegacyのRestoreFiles / RestoreFilesDirectを呼ばず、選択項目の復元と同じ計画経路を通ることを確認する
 */
void TestFileChangeModel::singleFileRestoreUsesStagedPlan()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;

    prepareComparisonRestore(&model, &fake, 100, 101,
                             QStringLiteral("t.... /etc/installed.conf\n"), QStringList());

    QSignalSpy completedSpy(&model, &FileChangeModel::restoreCompleted);

    model.restoreSingleFileFrom(QStringLiteral("/etc/installed.conf"), 100);
    fake.completeAllPendingOk();

    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Begin), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Stage), 1);
    QCOMPARE(fake.countKind(FakeRestorePlanTransport::Call::Commit), 1);

    const FakeRestorePlanTransport::Call &begin = fake.calls.first();
    QCOMPARE(begin.configName, QStringLiteral("root"));
    QCOMPARE(begin.snapshotNumber, 100);
    QCOMPARE(begin.counterpartNumber, 101);
    QCOMPARE(begin.restoreMode, QStringLiteral("direct"));

    for (const FakeRestorePlanTransport::Call &call : std::as_const(fake.calls)) {
        if (call.kind == FakeRestorePlanTransport::Call::Stage) {
            QCOMPARE(call.paths, QStringList{QStringLiteral("/etc/installed.conf")});
            QCOMPARE(call.changeTypes, QStringList{QStringLiteral("typechanged")});
        }
    }

    // 完了はサーバ側の終端シグナルで判定する
    QCOMPARE(completedSpy.count(), 0);
    fake.emitPlanFinished(fake.nextManifestId, QStringLiteral("completed"), QString());
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);
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
