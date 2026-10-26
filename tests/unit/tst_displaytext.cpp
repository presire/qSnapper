/**
 * @file tst_displaytext.cpp
 * @brief DisplayText::sanitize()の単体テスト
 *
 * パスの表示で、双方向制御文字によって復元や削除の対象を別のパスに見せかけられないことを固定する
 * 一覧の区切りに使う改行と、並びを入れ替えないLRM / RLM / ALMは残ることも確認する
 * QMLからの呼び出しと同じ経路 (JavaScriptの値からQStringへの変換) も確認する
 * 依存 : displaytext.h、Qt Qml (QJSEngine)、Qt Test
 */

#include <QtTest/QtTest>
#include <QJSEngine>
#include <QString>
#include "displaytext.h"

/**
 * @class TestDisplayText
 * @brief 表示用の文字列の無害化のテスト
 */
class TestDisplayText : public QObject
{
    Q_OBJECT

private slots:
    void bidiControlsAreReplaced_data();
    void bidiControlsAreReplaced();
    void otherCharactersAreKept_data();
    void otherCharactersAreKept();
    void qmlUndefinedAndNullBecomeEmpty();
};

/**
 * @brief 置き換え対象の境界となる文字の一覧を用意する
 */
void TestDisplayText::bidiControlsAreReplaced_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    const QChar r = QChar::ReplacementCharacter;
    QTest::newRow("LRE U+202A") << QString(QChar(0x202A)) << QString(r);
    QTest::newRow("RLO U+202E") << (QStringLiteral("a") + QChar(0x202E) + QStringLiteral("gpj.exe"))
                                << (QStringLiteral("a") + r + QStringLiteral("gpj.exe"));
    QTest::newRow("LRI U+2066") << QString(QChar(0x2066)) << QString(r);
    QTest::newRow("PDI U+2069") << QString(QChar(0x2069)) << QString(r);
    QTest::newRow("multiple") << (QString(QChar(0x2067)) + QStringLiteral("/etc") + QChar(0x202C))
                              << (QString(r) + QStringLiteral("/etc") + r);
}

/**
 * @brief 双方向制御文字がU+FFFDに置き換わることを確認する
 */
void TestDisplayText::bidiControlsAreReplaced()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    const DisplayText displayText;
    QCOMPARE(displayText.sanitize(input), expected);
}

/**
 * @brief 置き換えてはならない文字の一覧を用意する
 *
 * 範囲の直前と直後 (U+2029、U+202F、U+2065、U+206A) は、境界の取り違えを検出するために含める
 */
void TestDisplayText::otherCharactersAreKept_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("empty") << QString();
    QTest::newRow("newline separated paths") << QStringLiteral("/etc/a\n/etc/b");
    QTest::newRow("LRM U+200E") << QString(QChar(0x200E));
    QTest::newRow("RLM U+200F") << QString(QChar(0x200F));
    QTest::newRow("ALM U+061C") << QString(QChar(0x061C));
    QTest::newRow("U+2029") << QString(QChar(0x2029));
    QTest::newRow("U+202F") << QString(QChar(0x202F));
    QTest::newRow("U+2065") << QString(QChar(0x2065));
    QTest::newRow("U+206A") << QString(QChar(0x206A));
    QTest::newRow("japanese") << QStringLiteral("/home/ユーザ/文書.txt");
}

/**
 * @brief 双方向制御文字以外は変更しないことを確認する
 */
void TestDisplayText::otherCharactersAreKept()
{
    QFETCH(QString, input);

    const DisplayText displayText;
    QCOMPARE(displayText.sanitize(input), input);
}

/**
 * @brief QMLからundefinedやnullを渡した場合に、空文字列として扱うことを確認する
 *
 * 以前のJavaScript実装は、undefinedを空文字列として扱っていた
 * 文字列の"undefined"や"null"が表示されると、存在しないパスに見えるため同じ扱いを保つ
 */
void TestDisplayText::qmlUndefinedAndNullBecomeEmpty()
{
    QJSEngine engine;
    // 親を持たないQObjectはエンジンが所有して破棄する
    engine.globalObject().setProperty(QStringLiteral("DisplayText"), engine.newQObject(new DisplayText()));

    const QJSValue fromUndefined = engine.evaluate(QStringLiteral("DisplayText.sanitize(undefined)"));
    QVERIFY2(!fromUndefined.isError(), qPrintable(fromUndefined.toString()));
    QCOMPARE(fromUndefined.toString(), QString());

    const QJSValue fromNull = engine.evaluate(QStringLiteral("DisplayText.sanitize(null)"));
    QVERIFY2(!fromNull.isError(), qPrintable(fromNull.toString()));
    QCOMPARE(fromNull.toString(), QString());

    const QJSValue fromBidi = engine.evaluate(QStringLiteral("DisplayText.sanitize('a\\u202Eb')"));
    QVERIFY2(!fromBidi.isError(), qPrintable(fromBidi.toString()));
    QCOMPARE(fromBidi.toString(), QStringLiteral("a") + QChar(QChar::ReplacementCharacter) + QStringLiteral("b"));
}

QTEST_MAIN(TestDisplayText)
#include "tst_displaytext.moc"
