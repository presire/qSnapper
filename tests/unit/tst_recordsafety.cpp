// tst_recordsafety.cpp
//
// isRecordSafeText()の単体テスト
// 契約の詳細は、src/dbusservice/inputvalidator.hを参照
//
// 対象の脆弱性: D-Busサービスの変更一覧 (GetFileChanges系) とスナップショット一覧 (ListSnapshots系) はいずれも改行区切りのレコード列である
// パスやdescriptionに改行を混入できると1エントリが複数行に割れ、2行目以降が攻撃者の選んだ内容を持つ
// 独立レコードとしてクライアントに解釈される

#include <QtTest/QtTest>
#include <QString>
#include <QStringList>
#include "inputvalidator.h"

using qsnapper::security::isRecordSafeText;

class TestRecordSafety : public QObject
{
    Q_OBJECT

private slots:
    void accept_data();
    void accept();

    void reject_data();
    void reject();

    void carrierPathSplitsFileChangeRecord();
    void carrierDescriptionSplitsSnapshotRecord();
};

void TestRecordSafety::accept_data()
{
    QTest::addColumn<QString>("value");

    QTest::newRow("plain path")
        << QStringLiteral("/etc/important");

    QTest::newRow("path with spaces")
        << QStringLiteral("/home/alice/my documents/notes.txt");

    QTest::newRow("path with trailing space")
        << QStringLiteral("/home/alice/carrier ");

    QTest::newRow("empty value")
        << QString();

    QTest::newRow("multibyte description")
        << QStringLiteral("更新前のスナップショット");

    // UTF-8バイト列として解釈させる必要がある (QStringLiteralはUTF-16コード単位として扱うため)
    QTest::newRow("non-BMP description")
        << QString::fromUtf8("before update \xF0\x9F\x9A\x80");

    // カンマはCSVの列をずらすが、レコード境界そのものは壊さない
    // 正当なdescriptionにも現れ得るため、意図的に拒否しない
    QTest::newRow("comma in description")
        << QStringLiteral("install vim, git");

    QTest::newRow("equals in userdata value")
        << QStringLiteral("key=value=more");

    // U+2028 LINE SEPARATOR / U+2029 PARAGRAPH SEPARATOR:
    // C1範囲外のため拒否しない
    // クライアント側の split('\n') は正確にU+000Aのみで分割するため、これらはレコード境界を壊さない (契約としてここに固定する)
    QTest::newRow("U+2028 line separator")
        << (QStringLiteral("/etc/host") + QChar(0x2028) + QStringLiteral("s"));

    QTest::newRow("U+2029 paragraph separator")
        << (QStringLiteral("/etc/host") + QChar(0x2029) + QStringLiteral("s"));
}

void TestRecordSafety::accept()
{
    QFETCH(QString, value);
    QVERIFY2(isRecordSafeText(value),
             qPrintable(QStringLiteral("expected accept: %1").arg(value)));
}

void TestRecordSafety::reject_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<QString>("why");

    QTest::newRow("LF carrier directory name")
        << QStringLiteral("/home/alice/carrier\n+.... /etc/important")
        << QStringLiteral("newline splits one entry into two records");

    QTest::newRow("bare LF")
        << QStringLiteral("/etc/host\ns")
        << QStringLiteral("C0 LF");

    QTest::newRow("bare CR")
        << QStringLiteral("/etc/host\rs")
        << QStringLiteral("C0 CR");

    QTest::newRow("CRLF")
        << QStringLiteral("/etc/host\r\ns")
        << QStringLiteral("C0 CRLF");

    QTest::newRow("embedded NUL")
        << QString::fromUtf8("/etc/hosts\0evil", 15)
        << QStringLiteral("embedded NUL truncates syscall arguments");

    QTest::newRow("tab")
        << (QStringLiteral("/etc/host") + QChar(0x09) + QStringLiteral("s"))
        << QStringLiteral("C0 HT");

    QTest::newRow("escape")
        << (QStringLiteral("/etc/host") + QChar(0x1B) + QStringLiteral("s"))
        << QStringLiteral("C0 ESC enables terminal escape injection into logs");

    QTest::newRow("DEL")
        << (QStringLiteral("/etc/host") + QChar(0x7F) + QStringLiteral("s"))
        << QStringLiteral("DEL");

    QTest::newRow("C1 PAD")
        << (QStringLiteral("/etc/host") + QChar(0x80) + QStringLiteral("s"))
        << QStringLiteral("C1 control char");

    // U+0085 NEL: C1範囲内
    // 一部のテキスト処理では改行として扱われるため、レコード区切りを破壊しうる文字としてはC1検査で一括して拒否される
    QTest::newRow("C1 NEL")
        << (QStringLiteral("/etc/host") + QChar(0x85) + QStringLiteral("s"))
        << QStringLiteral("C1 NEL is within the rejected C1 range");

    QTest::newRow("C1 APC")
        << (QStringLiteral("/etc/host") + QChar(0x9F) + QStringLiteral("s"))
        << QStringLiteral("C1 control char upper bound");

    QTest::newRow("forged snapshot row in description")
        << QStringLiteral("update\n999,single,0,2026-01-01T00:00:00,0,number,evil,")
        << QStringLiteral("newline forges a whole snapshot row with an attacker-chosen number");
}

void TestRecordSafety::reject()
{
    QFETCH(QString, value);
    QFETCH(QString, why);
    QVERIFY2(!isRecordSafeText(value),
             qPrintable(QStringLiteral("expected reject (%1)").arg(why)));
}

/**
 * @brief carrierパスが行指向レコードを分割することを、実際の連結手順で示す
 *
 * 攻撃者はパス構成要素に '/' を含められないため、改行入りの「ディレクトリ」とその配下の通常階層でcarrierを作る
 * ここではサーバ側の連結 (statusStr + " " + path + "\n") を再現し、検証なしに連結すると
 * 偽の created エントリが生まれること、およびisRecordSafeText()がそれを止めることを確認する
 */
void TestRecordSafety::carrierPathSplitsFileChangeRecord()
{
    // ディレクトリ名は "carrier\n+.... " (末尾スペースを含む)
    // いずれもLinuxのファイル名として正当
    const QString carrierDirectory = QStringLiteral("carrier\n+.... ");
    const QString carrierPath =
        QStringLiteral("/home/alice/") + carrierDirectory + QStringLiteral("/etc/important");

    const QString serialized = QStringLiteral("+....") + QStringLiteral(" ") + carrierPath
                               + QStringLiteral("\n");
    const QStringList records = serialized.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    // 1エントリのはずが2レコードに割れ、2行目は完全に正当な書式のcreatedエントリになる
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0), QStringLiteral("+.... /home/alice/carrier"));
    QCOMPARE(records.at(1), QStringLiteral("+.... /etc/important"));

    // 偽造された2行目のパス自体はクリーンであり、パス検証では止まらない
    QVERIFY(isRecordSafeText(QStringLiteral("/etc/important")));

    // 止められるのはシリアライズ前の元のパスだけである
    QVERIFY(!isRecordSafeText(carrierPath));
}

/**
 * @brief 改行入りdescriptionが偽のスナップショット行を生むことを示す
 *
 * 偽のnumberは、クライアントからDeleteSnapshot / RollbackSnapshotへそのまま渡る
 */
void TestRecordSafety::carrierDescriptionSplitsSnapshotRecord()
{
    const QString forgedDescription =
        QStringLiteral("update\n999,single,0,2026-01-01T00:00:00,0,number,evil,");

    QString csv = QStringLiteral("number,type,pre-number,date,user,cleanup,description,userdata\n");
    csv += QStringLiteral("42,single,0,2026-01-01T00:00:00,0,number,") + forgedDescription
           + QStringLiteral(",\n");

    const QStringList lines = csv.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    // header + 本物1行 のはずが、偽行が1行増える
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines.at(2).startsWith(QStringLiteral("999,")));
    QCOMPARE(lines.at(2).split(QLatin1Char(',')).at(0).toInt(), 999);

    QVERIFY(!isRecordSafeText(forgedDescription));
}

QTEST_APPLESS_MAIN(TestRecordSafety)
#include "tst_recordsafety.moc"
