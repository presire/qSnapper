// tst_filepaths.cpp
// isPathWithinSnapshotRoot() / validateAbsoluteFilePath() の単体テスト。
// 契約の詳細は src/dbusservice/inputvalidator.h を参照。

#include <QtTest/QtTest>
#include <QString>

#include "inputvalidator.h"

using qsnapper::security::isPathWithinSnapshotRoot;
using qsnapper::security::validateAbsoluteFilePath;

class TestFilePaths : public QObject
{
    Q_OBJECT

private slots:
    void accept_data();
    void accept();

    void reject_data();
    void reject();

    void absolutePathValidation_data();
    void absolutePathValidation();

    void longPath();
};

void TestFilePaths::accept_data()
{
    QTest::addColumn<QString>("filePath");
    QTest::addColumn<QString>("snapshotRoot");

    QTest::newRow("A-2-1 normal file")
        << QStringLiteral("/.snapshots/42/snapshot/etc/hosts")
        << QStringLiteral("/.snapshots/42/snapshot");

    QTest::newRow("deep nested")
        << QStringLiteral("/.snapshots/42/snapshot/var/lib/qsnapper/data.bin")
        << QStringLiteral("/.snapshots/42/snapshot");

    QTest::newRow("A-2-5 root itself")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("/.snapshots/42/snapshot");

    QTest::newRow("trailing slash on root")
        << QStringLiteral("/.snapshots/42/snapshot/etc/hosts")
        << QStringLiteral("/.snapshots/42/snapshot/");
}

void TestFilePaths::accept()
{
    QFETCH(QString, filePath);
    QFETCH(QString, snapshotRoot);
    QVERIFY2(isPathWithinSnapshotRoot(filePath, snapshotRoot),
             qPrintable(QStringLiteral("expected within root: %1 / %2")
                            .arg(filePath, snapshotRoot)));
}

void TestFilePaths::reject_data()
{
    QTest::addColumn<QString>("filePath");
    QTest::addColumn<QString>("snapshotRoot");
    QTest::addColumn<QString>("why");

    QTest::newRow("A-2-2 relative path")
        << QStringLiteral("etc/hosts")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("relative paths must be rejected");

    QTest::newRow("A-2-3 traversal escape")
        << QStringLiteral("/.snapshots/42/snapshot/../../../etc/shadow")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("dotdot escapes snapshot root");

    QTest::newRow("A-2-4 prefix mismatch")
        << QStringLiteral("/etc/hosts")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("outside snapshot tree");

    QTest::newRow("A-2-6 NUL byte")
        << QString::fromUtf8("/.snapshots/42/snapshot/etc/hosts\0evil", 39)
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("embedded NUL");

    QTest::newRow("A-2-6 newline")
        << QStringLiteral("/.snapshots/42/snapshot/etc/hosts\nevil")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("embedded newline");

    QTest::newRow("A-2-10 canonical escape")
        << QStringLiteral("/.snapshots/42/snapshot/./../42-evil/etc/shadow")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("post-canonicalisation escapes root");

    QTest::newRow("sibling root trick")
        << QStringLiteral("/.snapshots/42/snapshot-evil/etc/hosts")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("prefix string-match must not allow sibling dirs");

    QTest::newRow("empty path")
        << QString()
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("empty input");

    QTest::newRow("empty root")
        << QStringLiteral("/.snapshots/42/snapshot/etc/hosts")
        << QString()
        << QStringLiteral("empty root must not act as universal match");

    // Round 2 review: 文字列プレフィクス境界の確認 (Comment 3 で新実装に変更)
    QTest::newRow("trailing char no separator")
        << QStringLiteral("/.snapshots/42/snapshotX")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("root suffix without '/' boundary must reject");

    // Round 2 review: 制御文字検査が filePath / snapshotRoot 双方に効くこと
    QTest::newRow("ctrl SOH in filePath")
        << (QStringLiteral("/.snapshots/42/snapshot/etc/host") + QChar(0x01) + QStringLiteral("s"))
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("C0 control char in tail must reject");

    QTest::newRow("DEL in filePath")
        << (QStringLiteral("/.snapshots/42/snapshot/etc/host") + QChar(0x7F) + QStringLiteral("s"))
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("DEL in tail must reject");

    QTest::newRow("C1 PAD in snapshotRoot")
        << QStringLiteral("/.snapshots/42/snapshot/etc/hosts")
        << (QStringLiteral("/.snapshots/42/snap") + QChar(0x80) + QStringLiteral("shot"))
        << QStringLiteral("C1 control char in root must reject");
}

void TestFilePaths::reject()
{
    QFETCH(QString, filePath);
    QFETCH(QString, snapshotRoot);
    QFETCH(QString, why);
    QVERIFY2(!isPathWithinSnapshotRoot(filePath, snapshotRoot),
             qPrintable(QStringLiteral("expected reject (%1): %2")
                            .arg(why, filePath)));
}

void TestFilePaths::absolutePathValidation_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("expected");

    QTest::newRow("absolute ok")
        << QStringLiteral("/etc/hosts")
        << true;
    QTest::newRow("empty")
        << QString()
        << false;
    QTest::newRow("relative")
        << QStringLiteral("etc/hosts")
        << false;
    QTest::newRow("newline")
        << QStringLiteral("/etc/hosts\nfoo")
        << false;
    QTest::newRow("nul")
        << QString::fromUtf8("/etc/hosts\0evil", 15)
        << false;
}

void TestFilePaths::absolutePathValidation()
{
    QFETCH(QString, path);
    QFETCH(bool, expected);
    QCOMPARE(validateAbsoluteFilePath(path), expected);
}

void TestFilePaths::longPath()
{
    // A-2-9: PATH_MAX (Linuxでは通常 4096) 超過
    const QString root = QStringLiteral("/.snapshots/42/snapshot");
    QString longTail(5000, QLatin1Char('a'));
    const QString bad = root + QStringLiteral("/") + longTail;
    QVERIFY2(!isPathWithinSnapshotRoot(bad, root),
             "paths exceeding PATH_MAX must be rejected");
    QVERIFY2(!validateAbsoluteFilePath(bad),
             "absolute path validator must reject paths exceeding PATH_MAX");
}

QTEST_APPLESS_MAIN(TestFilePaths)
#include "tst_filepaths.moc"
