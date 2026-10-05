// tst_fssnapshotstore.cpp
//
// FsSnapshotStore の単体テスト
//
// 対象の不具合: 非特権のGUIが/var/lib/qsnapperへ書き込もうとしており、権限的に必ず失敗していた
// 保存先がユーザー単位のデータディレクトリ (QStandardPaths::AppLocalDataLocation) であることと、
// 保存・読み込み・削除の往復が成立することを固定する

#include <QtTest/QtTest>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include "fssnapshotstore.h"

/**
 * @brief FsSnapshotStore の保存先と往復動作を検証するテストクラス
 */
class TestFsSnapshotStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void saveLoadCleanRoundTripInUserDataDirectory();
    void invalidPurposeIsRejected();

private:
    QString m_dataDir;
};

/**
 * @brief テスト用のデータディレクトリを準備する
 */
void TestFsSnapshotStore::initTestCase()
{
    // 実ユーザーのデータディレクトリを汚さないよう、Qtのテストモードの場所を使う
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("Presire"));
    QCoreApplication::setApplicationName(QStringLiteral("qSnapperFsSnapshotStoreTest"));

    m_dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QVERIFY(!m_dataDir.isEmpty());
    QVERIFY(!m_dataDir.startsWith(QStringLiteral("/var/lib")));
    QDir(m_dataDir).removeRecursively();
}

/**
 * @brief テスト用のデータディレクトリを削除する
 */
void TestFsSnapshotStore::cleanupTestCase()
{
    if (!m_dataDir.isEmpty()) {
        QDir(m_dataDir).removeRecursively();
    }
}

/**
 * @brief 保存・読み込み・削除がユーザー単位のデータディレクトリ上で成立することを確認する
 */
void TestFsSnapshotStore::saveLoadCleanRoundTripInUserDataDirectory()
{
    const QString purpose = QStringLiteral("zypp-test_1");
    const QString expectedPath = m_dataDir + QStringLiteral("/pre_snapshot_zypp-test_1.id");

    QCOMPARE(FsSnapshotStore::load(purpose), -1);
    QVERIFY(FsSnapshotStore::save(purpose, 42));
    QVERIFY(QFileInfo(expectedPath).isFile());
    QCOMPARE(FsSnapshotStore::load(purpose), 42);

    QVERIFY(FsSnapshotStore::clean(purpose));
    QVERIFY(!QFileInfo::exists(expectedPath));
    QCOMPARE(FsSnapshotStore::load(purpose), -1);
}

/**
 * @brief パス区切りなどを含む用途識別子が拒否され、ファイルが作られないことを確認する
 */
void TestFsSnapshotStore::invalidPurposeIsRejected()
{
    QVERIFY(!FsSnapshotStore::save(QStringLiteral("../escape"), 1));
    QVERIFY(!FsSnapshotStore::save(QString(), 1));
    QCOMPARE(FsSnapshotStore::load(QStringLiteral("a/b")), -1);
    QVERIFY(!QFileInfo::exists(m_dataDir + QStringLiteral("/../escape.id")));
}

QTEST_GUILESS_MAIN(TestFsSnapshotStore)
#include "tst_fssnapshotstore.moc"
