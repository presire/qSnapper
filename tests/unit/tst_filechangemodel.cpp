/**
 * @file tst_filechangemodel.cpp
 * @brief FileChangeModel単体テストの本体
 *
 * 共通ヘルパーとツリー構築テストを収め、QTEST_APPLESS_MAINで実行する
 */

#include "tst_filechangemodel.h"
#include <QtTest>
#include <QSignalSpy>
#include <QFileInfo>


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

void TestFileChangeModel::hasCheckedItemsTracksCheckState()
{
    TestableFileChangeModel model;
    QVERIFY(!model.hasCheckedItems());

    const QString output = QStringLiteral("c.... /etc/ld.so.cache\n"
                                          "+.... /usr/lib/new.so\n");
    model.setupModelData(output, false);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(!model.hasCheckedItems());

    // フォルダの下にあるファイルだけにチェックを入れた場合も、チェックありと判定されること
    model.setItemChecked(QStringLiteral("/etc/ld.so.cache"), true);
    QVERIFY(model.hasCheckedItems());
    model.setItemChecked(QStringLiteral("/etc/ld.so.cache"), false);
    QVERIFY(!model.hasCheckedItems());

    // フォルダにチェックを入れると配下のファイルにもチェックが入り、フォルダのチェックを外すとチェックなしに戻ること
    model.setItemChecked(QStringLiteral("/usr/"), true);
    QVERIFY(model.hasCheckedItems());
    model.setItemChecked(QStringLiteral("/usr/"), false);
    QVERIFY(!model.hasCheckedItems());

    // setupModelData()で一覧を読み直した後は、読み直す前に入れたチェックが残らないこと
    model.setItemChecked(QStringLiteral("/etc/ld.so.cache"), true);
    model.setupModelData(output, false);
    QVERIFY(!model.hasCheckedItems());
}

QTEST_APPLESS_MAIN(TestFileChangeModel)
