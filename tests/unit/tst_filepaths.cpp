// tst_filepaths.cpp
// レイヤA-2: RestoreFiles パス正規化の単体テスト (テスト計画 §2 A-2)
//
// 検査対象インタフェース (src/dbusservice/inputvalidator.h):
//
//   namespace qsnapper::security {
//       // filePath (絶対パス) が snapshotRoot 配下に収まっているか検証する。
//       // 仕様:
//       //   - filePath / snapshotRoot は絶対パスでなければ拒否
//       //   - filePath / snapshotRoot に制御文字 (C0 / DEL / C1) を含む場合は拒否
//       //   - PATH_MAX 超過は拒否
//       //   - std::filesystem::path::lexically_normal() で正規化した上で、
//       //     filePath が snapshotRoot を完全プレフィクスとして持ち、
//       //     かつ境界が「完全一致」または「直後がパス区切り '/'」であることを要求する
//       //     (sibling root trick "/a/b/snap" vs "/a/b/snap-evil" を弾くため、
//       //      単純な find()==0 ではなく境界チェックを必須としている)
//       //   - シンボリックリンク検証は呼び出し側で openat(O_NOFOLLOW) によって
//       //     行う (純粋関数のこのレイヤでは扱わない)
//       bool isPathWithinSnapshotRoot(const QString &filePath,
//                                     const QString &snapshotRoot);
//   }
//
// 注意:
//   symlink 関連のテスト (A-2-7 / A-2-8) は openat+O_NOFOLLOW のレイヤで扱うため、
//   本ファイルでは「正規化レベルで検知できるもの」のみ対象。symlink テストは
//   別途 tst_restore_integration.cpp で FS フィクスチャを用意して行う。

#include <QtTest/QtTest>
#include <QString>

#include "inputvalidator.h"

using qsnapper::security::isPathWithinSnapshotRoot;

class TestFilePaths : public QObject
{
    Q_OBJECT

private slots:
    void accept_data();
    void accept();

    void reject_data();
    void reject();

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

void TestFilePaths::longPath()
{
    // A-2-9: PATH_MAX (Linuxでは通常 4096) 超過
    const QString root = QStringLiteral("/.snapshots/42/snapshot");
    QString longTail(5000, QLatin1Char('a'));
    const QString bad = root + QStringLiteral("/") + longTail;
    QVERIFY2(!isPathWithinSnapshotRoot(bad, root),
             "paths exceeding PATH_MAX must be rejected");
}

QTEST_APPLESS_MAIN(TestFilePaths)
#include "tst_filepaths.moc"
