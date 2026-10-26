/**
 * @file tst_userdataparser.cpp
 * @brief qsnapper::userdata::parse()の単体テスト
 *
 * スナップショットのユーザデータ編集で、次の不具合が再発しないことを固定する
 * 値の前後の空白が黙って削除され、編集のたびに保存済みの値が変わる
 * 同じキーが複数あると、後の値で黙って上書きされる
 * 値に含まれるカンマで値が分割され、失われる
 * 依存 : userdataparser.h、Qt Test
 */

#include <QtTest/QtTest>
#include <QString>
#include "userdataparser.h"

using qsnapper::userdata::ParseError;
using qsnapper::userdata::ParseResult;
using qsnapper::userdata::parse;

/**
 * @class TestUserdataParser
 * @brief ユーザデータ解析のテスト
 */
class TestUserdataParser : public QObject
{
    Q_OBJECT

private slots:
    void emptyTextIsOkAndEmpty();
    void valuesArePreservedExactly();
    void keyIsTrimmedButValueIsNot();
    void commaIsNotASeparator();
    void duplicateKeyIsRejected();
    void duplicateKeyAfterTrimIsRejected();
    void missingSeparatorIsRejected();
    void emptyKeyIsRejected();
    void blankLinesAndCrlfAreHandled();
};

/**
 * @brief 空文字列が成功し、空のマップになることを確認する
 */
void TestUserdataParser::emptyTextIsOkAndEmpty()
{
    const ParseResult result = parse(QString());
    QVERIFY(result.ok);
    QVERIFY(result.values.isEmpty());
}

/**
 * @brief 値の前後の空白を削除せず、そのまま保持することを確認する
 *
 * 保存のたびに値が変化すると、利用者が編集していない項目まで差分として現れる
 */
void TestUserdataParser::valuesArePreservedExactly()
{
    const ParseResult result = parse(QStringLiteral("note=  keep spaces  \nempty="));
    QVERIFY(result.ok);
    QCOMPARE(result.values.value(QStringLiteral("note")).toString(), QStringLiteral("  keep spaces  "));
    QVERIFY(result.values.contains(QStringLiteral("empty")));
    QCOMPARE(result.values.value(QStringLiteral("empty")).toString(), QString());
}

/**
 * @brief キーの前後の空白だけを削除し、値の中の = は保持することを確認する
 *
 * 最初の = より後を値として扱わないと、値に含まれる = が失われる
 */
void TestUserdataParser::keyIsTrimmedButValueIsNot()
{
    const ParseResult result = parse(QStringLiteral("  key  = a = b "));
    QVERIFY(result.ok);
    QCOMPARE(result.values.size(), 1);
    QCOMPARE(result.values.value(QStringLiteral("key")).toString(), QStringLiteral(" a = b "));
}

/**
 * @brief カンマで値が分割されないことを確認する
 *
 * カンマを区切りとして扱うと、一覧や説明文の値が保存時に失われる
 */
void TestUserdataParser::commaIsNotASeparator()
{
    const ParseResult result = parse(QStringLiteral("list=a,b,c"));
    QVERIFY(result.ok);
    QCOMPARE(result.values.size(), 1);
    QCOMPARE(result.values.value(QStringLiteral("list")).toString(), QStringLiteral("a,b,c"));
}

/**
 * @brief 同じキーが複数あるとき、後勝ちで黙って上書きせずエラーになることを確認する
 *
 * 上書きを許すと、利用者が気付かないまま先に書いた値が失われる
 */
void TestUserdataParser::duplicateKeyIsRejected()
{
    const ParseResult result = parse(QStringLiteral("a=1\nb=2\na=3"));
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ParseError::DuplicateKey);
    QCOMPARE(result.line, 3);
    QCOMPARE(result.key, QStringLiteral("a"));
    QVERIFY(result.values.isEmpty());
}

/**
 * @brief 前後の空白だけが異なるキーも、重複として拒否することを確認する
 *
 * 正規化後のキーで重複を判定しないと、空白違いのキーが同じ設定を二重に持つ
 */
void TestUserdataParser::duplicateKeyAfterTrimIsRejected()
{
    const ParseResult result = parse(QStringLiteral("a=1\n a =2"));
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ParseError::DuplicateKey);
}

/**
 * @brief = を含まない行が、黙って捨てられずにエラーになることを確認する
 *
 * 捨てると、入力した設定が保存されていないことに利用者が気付かない
 */
void TestUserdataParser::missingSeparatorIsRejected()
{
    const ParseResult result = parse(QStringLiteral("a=1\nnoequals"));
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ParseError::MissingSeparator);
    QCOMPARE(result.line, 2);
}

/**
 * @brief キーが空の行がエラーになることを確認する
 *
 * 空キーを受け入れると、読み出し側が値を特定できず設定が無視される
 */
void TestUserdataParser::emptyKeyIsRejected()
{
    QCOMPARE(parse(QStringLiteral("=value")).error, ParseError::EmptyKey);
    QCOMPARE(parse(QStringLiteral("   =value")).error, ParseError::EmptyKey);
}

/**
 * @brief 空白だけの行を無視し、CRLFの改行コードでも値にCRが残らないことを確認する
 *
 * CRが値に残ると、保存した値がサーバ側の解釈と一致しない
 */
void TestUserdataParser::blankLinesAndCrlfAreHandled()
{
    const ParseResult result = parse(QStringLiteral("a=1\r\n\r\n   \r\nb=2\r\n"));
    QVERIFY(result.ok);
    QCOMPARE(result.values.size(), 2);
    QCOMPARE(result.values.value(QStringLiteral("a")).toString(), QStringLiteral("1"));
    QCOMPARE(result.values.value(QStringLiteral("b")).toString(), QStringLiteral("2"));
}

QTEST_MAIN(TestUserdataParser)
#include "tst_userdataparser.moc"
