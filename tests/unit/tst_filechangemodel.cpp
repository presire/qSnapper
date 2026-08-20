#include <QtTest>

#include "filechangemodel.h"

class TestableFileChangeModel : public FileChangeModel
{
public:
    using FileChangeModel::setupModelData;
};

class TestFileChangeModel : public QObject
{
    Q_OBJECT

private slots:
    void treeBuildRetainsSpacesDeduplicatesAndMarksParents();
    void flatBuildPreservesInputOrderAndExplicitDirectories();
    void treeBuildHandlesPrefixHeavyPaths();
};

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

QTEST_APPLESS_MAIN(TestFileChangeModel)
#include "tst_filechangemodel.moc"
