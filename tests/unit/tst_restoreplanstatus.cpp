/**
 * @file tst_restoreplanstatus.cpp
 * @brief qsnapper::restore::parseRestorePlanStatus()の単体テスト
 *
 * GetRestorePlanStatusのCSV応答を解析する関数を確認する
 * 依存 : csvrecord.hのquoteField、restoreplanstatus.h、Qt Test
 * 旧サーバの9フィールドと、サーバ拡張後の14フィールドの両方を受け付けること
 * 引用符付きフィールド (改行を含むスキップしたパスの一覧など) を正しく分割すること
 * 壊れた応答を無効として扱うことを固定する
 */

#include <QtTest/QtTest>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include "csvrecord.h"
#include "restoreplanstatus.h"

using qsnapper::csv::quoteField;
using qsnapper::restore::RestorePlanStatus;
using qsnapper::restore::parseRestorePlanStatus;

/**
 * @class TestRestorePlanStatus
 * @brief 復元計画の状態CSVの解析テスト
 */
class TestRestorePlanStatus : public QObject
{
    Q_OBJECT

private slots:
    void legacyNineFieldsAreAccepted();
    void extendedFourteenFieldsAreParsed();
    void skippedPathsKeepEmbeddedCommaAndQuote();
    void failedEntryIsReported();
    void malformedRecordsAreInvalid();
    void trailingNewlineIsIgnored();
    void variantMapExposesCounts();
};

/**
 * @brief 旧サーバの9フィールドの応答が有効で、拡張フィールドが既定値になることを確認する
 *
 * 拡張フィールドを必須にすると、更新前のサーバと通信できず状態表示が止まる回帰を防ぐ
 */
void TestRestorePlanStatus::legacyNineFieldsAreAccepted()
{
    const QString csv = QStringLiteral("abc,completed,10,10,10,direct,%1,42,%2")
                            .arg(quoteField(QStringLiteral("root")), quoteField(QString()));
    const RestorePlanStatus status = parseRestorePlanStatus(csv);
    QVERIFY(status.valid);
    QVERIFY(!status.hasExtended);
    QCOMPARE(status.state, QStringLiteral("completed"));
    QCOMPARE(status.totalEntries, 10);
    QCOMPARE(status.processed, 10);
    QCOMPARE(status.snapshotNumber, 42);
    QCOMPARE(status.configName, QStringLiteral("root"));
    QCOMPARE(status.deletedCount, 0);
    QCOMPARE(status.skippedCount, 0);
    QCOMPARE(status.failedIndex, -1);
    QVERIFY(status.failedPath.isEmpty());
    QVERIFY(status.skippedPaths.isEmpty());
    // スキップが無い場合、適用数は処理済み数と一致する
    QCOMPARE(status.appliedCount(), 10);
}

/**
 * @brief 14フィールドの応答から、削除数とスキップ数とスキップしたパスを取り出せることを確認する
 *
 * 追加フィールドを読み飛ばすと、GUIに削除件数とスキップ理由が出ない回帰を防ぐ
 */
void TestRestorePlanStatus::extendedFourteenFieldsAreParsed()
{
    const QString skipped = QStringLiteral("/run/fifo\n/dev/null2");
    const QString csv = QStringLiteral("abc,completed,10,10,10,yast,%1,7,%2,3,2,-1,%3,%4")
                            .arg(quoteField(QStringLiteral("home")),
                                 quoteField(QString()),
                                 quoteField(QString()),
                                 quoteField(skipped));
    const RestorePlanStatus status = parseRestorePlanStatus(csv);
    QVERIFY(status.valid);
    QVERIFY(status.hasExtended);
    QCOMPARE(status.mode, QStringLiteral("yast"));
    QCOMPARE(status.deletedCount, 3);
    QCOMPARE(status.skippedCount, 2);
    QCOMPARE(status.failedIndex, -1);
    QCOMPARE(status.skippedPaths, (QStringList{QStringLiteral("/run/fifo"), QStringLiteral("/dev/null2")}));
    // 処理済み10件のうちスキップ2件は適用されていない
    QCOMPARE(status.appliedCount(), 8);
}

/**
 * @brief パスにカンマと引用符が含まれても、フィールドがずれないことを確認する
 *
 * 引用符を解釈しないと後続の列が1つずつずれ、無関係な値を削除件数や失敗位置として表示してしまう
 */
void TestRestorePlanStatus::skippedPathsKeepEmbeddedCommaAndQuote()
{
    const QString tricky = QStringLiteral("/tmp/a,b\"c");
    const QString csv = QStringLiteral("abc,completed,1,1,1,direct,%1,1,%2,0,1,-1,%3,%4")
                            .arg(quoteField(QStringLiteral("root")),
                                 quoteField(QString()),
                                 quoteField(QString()),
                                 quoteField(tricky));
    const RestorePlanStatus status = parseRestorePlanStatus(csv);
    QVERIFY(status.valid);
    QCOMPARE(status.skippedPaths, QStringList{tricky});
}

/**
 * @brief 失敗したエントリの位置とパスとエラー文言を取り出せることを確認する
 *
 * 失敗位置を捨てると、どのファイルで復元が止まったかを利用者に示せない
 */
void TestRestorePlanStatus::failedEntryIsReported()
{
    const QString csv = QStringLiteral("abc,failed,10,4,4,direct,%1,1,%2,1,0,3,%3,%4")
                            .arg(quoteField(QStringLiteral("root")),
                                 quoteField(QStringLiteral("copy failed")),
                                 quoteField(QStringLiteral("/etc/app.conf")),
                                 quoteField(QString()));
    const RestorePlanStatus status = parseRestorePlanStatus(csv);
    QVERIFY(status.valid);
    QCOMPARE(status.state, QStringLiteral("failed"));
    QCOMPARE(status.failedIndex, 3);
    QCOMPARE(status.failedPath, QStringLiteral("/etc/app.conf"));
    QCOMPARE(status.lastError, QStringLiteral("copy failed"));
    QCOMPARE(status.deletedCount, 1);
}

/**
 * @brief フィールド不足と数値の不正と空文字列を無効として扱うことを確認する
 *
 * 壊れた応答を有効として受け入れると、存在しない復元結果を完了として表示してしまう
 */
void TestRestorePlanStatus::malformedRecordsAreInvalid()
{
    QVERIFY(!parseRestorePlanStatus(QString()).valid);
    QVERIFY(!parseRestorePlanStatus(QStringLiteral("abc,completed,10")).valid);
    // totalEntriesが数値ではない
    QVERIFY(!parseRestorePlanStatus(QStringLiteral("abc,completed,x,1,1,direct,\"root\",1,\"\"")).valid);
    // 閉じていない引用符
    QVERIFY(!parseRestorePlanStatus(QStringLiteral("abc,completed,1,1,1,direct,\"root,1,\"\"")).valid);
}

/**
 * @brief 末尾の改行コードがあっても解析できることを確認する
 *
 * サーバが行末に改行を付けた場合に無効とすると、状態が完了へ遷移しない
 */
void TestRestorePlanStatus::trailingNewlineIsIgnored()
{
    const QString csv = QStringLiteral("abc,running,5,2,2,direct,\"root\",9,\"\"\r\n");
    const RestorePlanStatus status = parseRestorePlanStatus(csv);
    QVERIFY(status.valid);
    QCOMPARE(status.state, QStringLiteral("running"));
    QCOMPARE(status.snapshotNumber, 9);
}

/**
 * @brief QMLへ渡すQVariantMapに、表示に必要なキーが含まれることを確認する
 *
 * キー名が変わると、QML側のバインディングが空の復元結果を表示する
 */
void TestRestorePlanStatus::variantMapExposesCounts()
{
    const QString csv = QStringLiteral("abc,completed,6,6,6,direct,\"root\",1,\"\",2,1,-1,\"\",\"/x\"");
    const QVariantMap map = parseRestorePlanStatus(csv).toVariantMap();
    QCOMPARE(map.value(QStringLiteral("valid")).toBool(), true);
    QCOMPARE(map.value(QStringLiteral("deletedCount")).toInt(), 2);
    QCOMPARE(map.value(QStringLiteral("skippedCount")).toInt(), 1);
    QCOMPARE(map.value(QStringLiteral("appliedCount")).toInt(), 5);
    QCOMPARE(map.value(QStringLiteral("skippedPaths")).toStringList(), QStringList{QStringLiteral("/x")});
}

QTEST_MAIN(TestRestorePlanStatus)
#include "tst_restoreplanstatus.moc"
