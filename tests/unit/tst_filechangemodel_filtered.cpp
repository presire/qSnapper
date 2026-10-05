/**
 * @file tst_filechangemodel_filtered.cpp
 * @brief 絞り込み比較と多層防御のテスト
 *
 * 対カレント絞り込みと改行注入に対する多層防御を検証する
 */

#include "tst_filechangemodel.h"
#include <QtTest>
#include <QSignalSpy>
#include <QFileInfo>


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
