/**
 * @file tst_btrfsinfo.cpp
 * @brief src/dbusservice/btrfsinfo.hの単体テスト
 *
 * パス判定とCSV整形は実ファイルシステムを必要としないため、その場で値を組み立てて検証する
 * openBtrfsDirectory()はQTemporaryDir内の実在パスとsymlinkを使い、ディレクトリ型とsymlink拒否を確認する
 * Btrfs固有のioctlとサブボリューム列挙はroot権限と実Btrfsを必要とするため、ここでは呼び出さない
 */

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <linux/magic.h>
#include <sys/statfs.h>

#include "btrfsinfo.h"

using namespace qsnapper::btrfs;

/**
 * @class TestBtrfsInfo
 * @brief Btrfs情報の読み取り判定と表示用CSVの整形を検証する
 */
class TestBtrfsInfo : public QObject
{
    Q_OBJECT

private slots:
    void isSnapperSnapshotPath_data();
    void isSnapperSnapshotPath();
    void formatFilesystemUsageCsv();
    void formatFilesystemUsageCsvEmpty();
    void formatSubvolumeListCsv();
    void openBtrfsDirectoryRejectsRelativePath();
    void openBtrfsDirectoryRejectsMissingPath();
    void openBtrfsDirectoryRejectsSymlink();
    void openBtrfsDirectoryRejectsRegularFile();
    void openBtrfsDirectoryRejectsNonBtrfsDirectory();
};

void TestBtrfsInfo::isSnapperSnapshotPath_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<bool>("expected");

    QTest::newRow("plain snapshot") << QByteArrayLiteral(".snapshots/12/snapshot") << true;
    QTest::newRow("at prefix") << QByteArrayLiteral("@/.snapshots/1/snapshot") << true;
    QTest::newRow("home prefix") << QByteArrayLiteral("home/.snapshots/3/snapshot") << true;
    QTest::newRow("multi digit") << QByteArrayLiteral(".snapshots/12345/snapshot") << true;
    QTest::newRow("leading zero") << QByteArrayLiteral(".snapshots/0/snapshot") << true;

    QTest::newRow("snapshots only") << QByteArrayLiteral(".snapshots") << false;
    QTest::newRow("number only") << QByteArrayLiteral(".snapshots/12") << false;
    QTest::newRow("trailing component") << QByteArrayLiteral(".snapshots/12/snapshot/x") << false;
    QTest::newRow("non digit") << QByteArrayLiteral(".snapshots/1a/snapshot") << false;
    QTest::newRow("empty number") << QByteArrayLiteral(".snapshots//snapshot") << false;
    QTest::newRow("trailing slash") << QByteArrayLiteral(".snapshots/12/snapshot/") << false;
    QTest::newRow("similar component") << QByteArrayLiteral("foo.snapshots/12/snapshot") << false;
    QTest::newRow("similar last component")
            << QByteArrayLiteral(".snapshots/12/snapshot-backup") << false;
    QTest::newRow("empty") << QByteArray() << false;
}

void TestBtrfsInfo::isSnapperSnapshotPath()
{
    QFETCH(QByteArray, path);
    QFETCH(bool, expected);

    QCOMPARE(qsnapper::btrfs::isSnapperSnapshotPath(path), expected);
}

void TestBtrfsInfo::formatFilesystemUsageCsv()
{
    FilesystemUsage usage;
    usage.numDevices = 2;
    usage.nodesize = 16384;
    usage.sectorsize = 4096;

    SpaceEntry metadataSpace = {};
    metadataSpace.flags = 1;
    metadataSpace.totalBytes = 100;
    metadataSpace.usedBytes = 40;
    usage.spaces.append(metadataSpace);

    SpaceEntry dataSpace = {};
    dataSpace.flags = 5;
    dataSpace.totalBytes = 200;
    dataSpace.usedBytes = 150;
    usage.spaces.append(dataSpace);

    DeviceEntry firstDevice = {};
    firstDevice.devid = 1;
    firstDevice.totalBytes = 1000;
    firstDevice.bytesUsed = 400;
    usage.devices.append(firstDevice);

    DeviceEntry secondDevice = {};
    secondDevice.devid = 2;
    secondDevice.totalBytes = 2000;
    secondDevice.bytesUsed = 1600;
    usage.devices.append(secondDevice);

    const QString expected = QStringLiteral("kind,flags,total,used\n"
                                            "info,16384,4096,2\n"
                                            "space,1,100,40\n"
                                            "space,5,200,150\n"
                                            "device,1,1000,400\n"
                                            "device,2,2000,1600");
    const QString csv = qsnapper::btrfs::formatFilesystemUsageCsv(usage);

    QCOMPARE(csv, expected);
    QVERIFY(!csv.endsWith(QLatin1Char('\n')));
}

void TestBtrfsInfo::formatFilesystemUsageCsvEmpty()
{
    const FilesystemUsage usage;
    const QString csv = qsnapper::btrfs::formatFilesystemUsageCsv(usage);

    QCOMPARE(csv, QStringLiteral("kind,flags,total,used\ninfo,0,0,0"));
    QVERIFY(!csv.endsWith(QLatin1Char('\n')));
}

void TestBtrfsInfo::formatSubvolumeListCsv()
{
    QVector<SubvolumeEntry> entries;

    SubvolumeEntry snapshot = {};
    snapshot.id = 7;
    snapshot.parentId = 5;
    snapshot.otimeSec = 1700000000;
    snapshot.readOnly = true;
    snapshot.path = QByteArrayLiteral(".snapshots/7/snapshot");
    entries.append(snapshot);

    SubvolumeEntry quoted = {};
    quoted.id = 8;
    quoted.parentId = 5;
    quoted.otimeSec = 1700000001;
    quoted.readOnly = false;
    quoted.path = QByteArrayLiteral("data,with\"quote");
    entries.append(quoted);

    SubvolumeEntry controlled = {};
    controlled.id = 9;
    controlled.parentId = 5;
    controlled.otimeSec = 0;
    controlled.readOnly = false;
    controlled.path = QByteArrayLiteral("bad\x01name");
    entries.append(controlled);

    QString expected = QStringLiteral("truncated,1\n"
                                      "id,parent_id,created,readonly,snapper,path\n"
                                      "7,5,1700000000,1,1,.snapshots/7/snapshot\n"
                                      "8,5,1700000001,0,0,\"data,with\"\"quote\"\n"
                                      "9,5,0,0,0,bad");
    expected += QChar(0xFFFD);
    expected += QStringLiteral("name");

    const QString csv = qsnapper::btrfs::formatSubvolumeListCsv(entries, true);

    QCOMPARE(csv, expected);
    QVERIFY(!csv.endsWith(QLatin1Char('\n')));
}

void TestBtrfsInfo::openBtrfsDirectoryRejectsRelativePath()
{
    QCOMPARE(openBtrfsDirectory(QStringLiteral("relative/path")), -1);
    QCOMPARE(openBtrfsDirectory(QString()), -1);
    QCOMPARE(openBtrfsDirectory(QStringLiteral(".")), -1);
}

void TestBtrfsInfo::openBtrfsDirectoryRejectsMissingPath()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QCOMPARE(openBtrfsDirectory(tempDir.filePath(QStringLiteral("missing"))), -1);
}

void TestBtrfsInfo::openBtrfsDirectoryRejectsSymlink()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString targetDir = tempDir.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(targetDir));

    const QString linkPath = tempDir.filePath(QStringLiteral("link"));
    QVERIFY(QFile::link(targetDir, linkPath));

    QCOMPARE(openBtrfsDirectory(linkPath), -1);
    QCOMPARE(openBtrfsDirectory(linkPath + QStringLiteral("/child")), -1);
}

void TestBtrfsInfo::openBtrfsDirectoryRejectsRegularFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath(QStringLiteral("regular"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QCOMPARE(openBtrfsDirectory(filePath), -1);
}

void TestBtrfsInfo::openBtrfsDirectoryRejectsNonBtrfsDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray encodedTempPath = QFile::encodeName(tempDir.path());
    struct statfs status = {};
    QVERIFY(::statfs(encodedTempPath.constData(), &status) == 0);

    if (static_cast<unsigned long>(status.f_type) == static_cast<unsigned long>(BTRFS_SUPER_MAGIC)) {
        QSKIP("一時ディレクトリがBtrfs上のため、非Btrfsの拒否を検証できない");
    }

    QCOMPARE(openBtrfsDirectory(tempDir.path()), -1);
}

QTEST_GUILESS_MAIN(TestBtrfsInfo)

#include "tst_btrfsinfo.moc"
