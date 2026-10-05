// tst_csvrecord.cpp
//
// qsnapper::csv::quoteField() / splitRecord() の単体テスト
// 契約の詳細は、include/csvrecord.hを参照
//
// 対象の脆弱性: スナップショット一覧CSVをクライアントが split(',') で分割していたため、
// カンマを含む正当なdescription等で後続の列がずれ、表示が壊れていた
// producer (quoteField) とconsumer (splitRecord) は同じヘッダを使い対称性を構成によって保証する
// また、quoteは表示正しさのための層であり、制御文字の拒否 (isRecordSafeText, fail-closed) の
// 代替ではないことをregressionテストで固定する

#include <QtTest/QtTest>
#include <QString>
#include <QStringList>
#include "csvrecord.h"
#include "inputvalidator.h"

using qsnapper::csv::quoteField;
using qsnapper::csv::splitRecord;
using qsnapper::security::isRecordSafeText;

class TestCsvRecord : public QObject
{
    Q_OBJECT

private slots:
    void commaInDescriptionRoundTrips();
    void doubleQuotesRoundTrip();
    void whitespacePreservedVerbatim();
    void emptyAndQuotedEmptyFields();
    void userdataCommaInKeySurvivesAsOneField();
    void userdataCommaInValueSurvivesAsOneField();
    void legacyUnquotedMatchesNaiveSplit();
    void unclosedQuoteIsRejected_data();
    void unclosedQuoteIsRejected();
    void controlCharactersStillRejected();
    void fullRecordRoundTrip();
    void createdSnapshotNumberIsTakenFromReply();
    void malformedCreateReplyIsRejected();
};

void TestCsvRecord::commaInDescriptionRoundTrips()
{
    const QString description = QStringLiteral("install vim, git");
    const QString quoted = quoteField(description);

    // カンマを含むフィールドはquoteされる
    QCOMPARE(quoted, QStringLiteral("\"install vim, git\""));

    // quoteしてからsplitすると、必ず1フィールドに戻る
    const QStringList fields = splitRecord(quoted);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.at(0), description);
}

void TestCsvRecord::doubleQuotesRoundTrip()
{
    // 二重引用符を含む値
    const QString withQuotes = QStringLiteral("he said \"hi\"");
    QStringList fields = splitRecord(quoteField(withQuotes));
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.at(0), withQuotes);

    // 引用符のみの値
    const QString onlyQuotes = QStringLiteral("\"\"\"");
    fields = splitRecord(quoteField(onlyQuotes));
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.at(0), onlyQuotes);

    // 二重引用符は "" へエスケープされる
    QCOMPARE(quoteField(withQuotes), QStringLiteral("\"he said \"\"hi\"\"\""));
}

void TestCsvRecord::whitespacePreservedVerbatim()
{
    // 前後の空白はquote対象外であり、パース後も文字通り保持される
    const QStringList specialChars = {QStringLiteral(" leading"), QStringLiteral("trailing "), QStringLiteral("  both  ")};
    for (const QString &value : specialChars) {
        const QStringList fields = splitRecord(quoteField(value));
        QCOMPARE(fields.size(), 1);
        QCOMPARE(fields.at(0), value);
    }

    // 無quote出力でも空白は保持される (旧サーバー互換)
    const QStringList legacy = splitRecord(QStringLiteral("a, b ,c"));
    QCOMPARE(legacy.size(), 3);
    QCOMPARE(legacy.at(1), QStringLiteral(" b "));
}

void TestCsvRecord::emptyAndQuotedEmptyFields()
{
    // 空フィールド
    QStringList fields = splitRecord(QStringLiteral("a,,b"));
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(1), QString());

    // 末尾の空フィールド
    fields = splitRecord(QStringLiteral("a,"));
    QCOMPARE(fields.size(), 2);
    QCOMPARE(fields.at(1), QString());

    // 引用符のみの空フィールド ("")
    fields = splitRecord(QStringLiteral("a,\"\",b"));
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(1), QString());
    QCOMPARE(fields.at(2), QStringLiteral("b"));

    // 空レコードは1つの空フィールド (split(',') と同一)
    fields = splitRecord(QString());
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.at(0), QString());
}

void TestCsvRecord::userdataCommaInKeySurvivesAsOneField()
{
    // keyにカンマを含むuserdataペアは1フィールドとしてquoteされ、
    // パース後も最初の '=' で正しく分割される
    const QString pair = QStringLiteral("my,key=value");
    const QString quoted = quoteField(pair);
    QVERIFY(quoted != pair);

    const QStringList fields = splitRecord(quoted);
    QCOMPARE(fields.size(), 1);

    const int eqIdx = fields.at(0).indexOf('=');
    QVERIFY(eqIdx > 0);
    QCOMPARE(fields.at(0).left(eqIdx), QStringLiteral("my,key"));
    QCOMPARE(fields.at(0).mid(eqIdx + 1), QStringLiteral("value"));
}

void TestCsvRecord::userdataCommaInValueSurvivesAsOneField()
{
    // valueにカンマを含むuserdataペアも同様
    const QString pair = QStringLiteral("key=va,lue");
    const QString quoted = quoteField(pair);
    QVERIFY(quoted != pair);

    const QStringList fields = splitRecord(quoted);
    QCOMPARE(fields.size(), 1);

    const int eqIdx = fields.at(0).indexOf('=');
    QVERIFY(eqIdx > 0);
    QCOMPARE(fields.at(0).left(eqIdx), QStringLiteral("key"));
    QCOMPARE(fields.at(0).mid(eqIdx + 1), QStringLiteral("va,lue"));
}

void TestCsvRecord::legacyUnquotedMatchesNaiveSplit()
{
    // 特殊文字を含まない入力はquoteされず、旧 split(',') と同一に分割される
    const QStringList legacyLines = {
        QStringLiteral("1,single,,2024-01-01T00:00:00,0,number,install vim,ud=1"),
        QStringLiteral("42,pre,41,2024-06-30T12:34:56,1000,timeline,before update,important=yes"),
        QStringLiteral("7,post,6,2024-06-30T12:40:00,0,,empty cleanup,"),
    };

    for (const QString &line : legacyLines) {
        QCOMPARE(splitRecord(line), line.split(','));
    }

    // 無quoteの旧出力に散在する " も文字通り保持される
    const QString strayQuote = QStringLiteral("a\"b,c\"d");
    const QStringList fields = splitRecord(strayQuote);
    QCOMPARE(fields, strayQuote.split(','));
}

/**
 * @brief 閉じていない引用符を含む入力のデータ
 */
void TestCsvRecord::unclosedQuoteIsRejected_data()
{
    QTest::addColumn<QString>("record");

    QTest::newRow("only opening quote")    << QStringLiteral("\"");
    QTest::newRow("single field")          << QStringLiteral("\"abc");
    QTest::newRow("last field")            << QStringLiteral("a,\"b,c");
    QTest::newRow("ends with escaped")     << QStringLiteral("\"a\"\"");
    QTest::newRow("full snapshot record")  << QStringLiteral("42,single,,2024-06-30T12:34:56,0,number,\"install vim, git");
}

/**
 * @brief 閉じていない引用符を含む行は、途中で切れた値として受け入れずに空のリストを返すことを確認する
 */
void TestCsvRecord::unclosedQuoteIsRejected()
{
    QFETCH(QString, record);

    QVERIFY(splitRecord(record).isEmpty());
}

void TestCsvRecord::controlCharactersStillRejected()
{
    // quoteは表示正しさのための層であり、制御文字のfail-closed拒否の代替ではない
    // LF / CRはC0、0x85はC1に含まれるため、isRecordSafeText()は依然として拒否する
    const QStringList controlValues = {
        QStringLiteral("line1\nline2"),
        QStringLiteral("line1\rline2"),
        QStringLiteral("tab\there"),
        QString::fromUtf8("nul\0here", 8),
        QString(QChar(0x007f)),
        QString(QChar(0x0085)),
    };

    for (const QString &value : controlValues) {
        QVERIFY2(!isRecordSafeText(value),
                 qPrintable(QStringLiteral("expected reject: %1").arg(QString::fromUtf8(value.toUtf8().toHex()))));
    }
}

void TestCsvRecord::fullRecordRoundTrip()
{
    // 実際のレコード列と同じカラム順でquote / splitの往復を検証する
    const int number = 42;
    const QString type = QStringLiteral("pre");
    const int preNumber = 41;
    const QString date = QStringLiteral("2024-06-30T12:34:56");
    const QString user = QStringLiteral("1000");
    const QString cleanup = QStringLiteral("number,extra");
    const QString description = QStringLiteral("install vim, git and \"htop\"");
    const QString pair1 = QStringLiteral("important=yes");
    const QString pair2 = QStringLiteral("note=snapshot of /home, all users");

    const QStringList values{
        QString::number(number),
        type,
        QString::number(preNumber),
        date,
        user,
        quoteField(cleanup),
        quoteField(description),
        quoteField(pair1),
        quoteField(pair2),
    };

    const QString record = values.join(QLatin1Char(','));
    const QStringList fields = splitRecord(record);

    QCOMPARE(fields.size(), 9);
    QCOMPARE(fields.at(0), QString::number(number));
    QCOMPARE(fields.at(1), type);
    QCOMPARE(fields.at(2), QString::number(preNumber));
    QCOMPARE(fields.at(3), date);
    QCOMPARE(fields.at(4), user);
    QCOMPARE(fields.at(5), cleanup);
    QCOMPARE(fields.at(6), description);
    QCOMPARE(fields.at(7), pair1);
    QCOMPARE(fields.at(8), pair2);
}

/**
 * @brief CreateSnapshotの応答から、一覧の末尾ではなく応答中の番号で作成分を特定することを確認する
 */
void TestCsvRecord::createdSnapshotNumberIsTakenFromReply()
{
    const QString header = QStringLiteral("number,type,pre-number,date,user,cleanup,description,userdata\n");

    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(
                 header + QStringLiteral("42,single,-1,2026-10-05T12:00:00,0,,\"a, b\",\n")), 42);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(
                 header + QStringLiteral("7,post,6,2026-10-05T12:00:00,0,number,x,key=value")), 7);
}

/**
 * @brief 形式が不正な応答では番号を推測しないことを確認する
 */
void TestCsvRecord::malformedCreateReplyIsRejected()
{
    const QString header = QStringLiteral("number,type,pre-number,date,user,cleanup,description,userdata\n");

    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(QString()), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(header), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(QStringLiteral("42,single\n")), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(header + QStringLiteral("abc,single\n")), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(header + QStringLiteral("0,single\n")), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(
                 header + QStringLiteral("41,single\n42,single\n")), -1);

    // 引用符が閉じていないレコード行・ヘッダ行
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(
                 header + QStringLiteral("42,single,-1,2026-10-05T12:00:00,0,,\"a, b\n")), -1);
    QCOMPARE(qsnapper::csv::parseCreatedSnapshotNumber(
                 QStringLiteral("number,\"type\n42,single\n")), -1);
}

QTEST_MAIN(TestCsvRecord)
#include "tst_csvrecord.moc"
