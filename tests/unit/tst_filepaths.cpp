/**
 * @file tst_filepaths.cpp
 * @brief isPathWithinSnapshotRoot() と validateAbsoluteFilePath() の単体テスト
 *
 * 契約の詳細は src/dbusservice/inputvalidator.h を参照する
 * スナップショットルート配下判定と絶対パス検証が、細工された入力に対して
 * 正しく受け入れ・拒否することを確認する
 */

#include <QtTest/QtTest>
#include <QString>

#include "inputvalidator.h"

using qsnapper::security::isPathWithinSnapshotRoot;
using qsnapper::security::validateAbsoluteFilePath;

/**
 * @class TestFilePaths
 * @brief ファイルパス検証関数の境界条件を確認するテストスイート
 */
class TestFilePaths : public QObject
{
    Q_OBJECT

private slots:
    /** @brief ルート配下として受け入れるべき入力のデータ行を定義する */
    void accept_data();
    /** @brief ルート配下の入力が受け入れられることを確認する */
    void accept();

    /** @brief 拒否すべき入力のデータ行を定義する */
    void reject_data();
    /** @brief ルート外または不正な入力が拒否されることを確認する */
    void reject();

    /** @brief 絶対パス検証の入力と期待結果のデータ行を定義する */
    void absolutePathValidation_data();
    /** @brief 絶対パス検証が期待どおりの真偽値を返すことを確認する */
    void absolutePathValidation();

    /** @brief PATH_MAX を超える長大パスが拒否されることを確認する */
    void longPath();
};

/**
 * @brief isPathWithinSnapshotRoot() が受け入れるべき組み合わせを定義する
 *
 * 通常ファイル、深いネスト、ルート自身、ルート末尾のスラッシュを確認する
 */
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

/**
 * @brief isPathWithinSnapshotRoot() がルート配下のパスを受け入れることを確認する
 *
 * 全データ行で true が返ることを検証する
 */
void TestFilePaths::accept()
{
    QFETCH(QString, filePath);
    QFETCH(QString, snapshotRoot);
    QVERIFY2(isPathWithinSnapshotRoot(filePath, snapshotRoot),
             qPrintable(QStringLiteral("expected within root: %1 / %2")
                            .arg(filePath, snapshotRoot)));
}

/**
 * @brief isPathWithinSnapshotRoot() が拒否すべき組み合わせを定義する
 *
 * 相対パス、ドットドットによる脱出、プレフィクス不一致、NUL バイト、改行
 * 正規化後の脱出、兄弟ディレクトリのトリック、空入力、制御文字などを含む
 */
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

    // Round 2 review: 文字列プレフィクスの境界を確認する
    // Comment 3 で新実装に変更した
    QTest::newRow("trailing char no separator")
        << QStringLiteral("/.snapshots/42/snapshotX")
        << QStringLiteral("/.snapshots/42/snapshot")
        << QStringLiteral("root suffix without '/' boundary must reject");

    // Round 2 review: 制御文字検査が filePath と snapshotRoot の双方に効くことを確認する
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

/**
 * @brief isPathWithinSnapshotRoot() が不正なパスを拒否することを確認する
 *
 * 全データ行で false が返ることを検証する
 */
void TestFilePaths::reject()
{
    QFETCH(QString, filePath);
    QFETCH(QString, snapshotRoot);
    QFETCH(QString, why);
    QVERIFY2(!isPathWithinSnapshotRoot(filePath, snapshotRoot),
             qPrintable(QStringLiteral("expected reject (%1): %2")
                            .arg(why, filePath)));
}

/**
 * @brief validateAbsoluteFilePath() の入力と期待結果を定義する
 *
 * 絶対パス、空文字列、相対パス、改行、NUL バイトを網羅する
 */
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

/**
 * @brief validateAbsoluteFilePath() が期待どおりの真偽値を返すことを確認する
 */
void TestFilePaths::absolutePathValidation()
{
    QFETCH(QString, path);
    QFETCH(bool, expected);
    QCOMPARE(validateAbsoluteFilePath(path), expected);
}

/**
 * @brief PATH_MAX を超えるパスが両関数で拒否されることを確認する
 *
 * A-2-9 として Linux の PATH_MAX 4096 を超える 5000 文字の末尾を付与する
 */
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
