// tst_configname.cpp
// validateConfigName() の単体テスト。
// 契約の詳細は src/dbusservice/inputvalidator.h を参照。

#include <QtTest/QtTest>
#include <QString>

#include "inputvalidator.h"

using qsnapper::security::validateConfigName;

class TestConfigName : public QObject
{
    Q_OBJECT

private slots:
    // 受理されるべき
    void accept_data();
    void accept();

    // 拒否されるべき
    void reject_data();
    void reject();

    // 長さ境界値
    void lengthBoundary();
};

void TestConfigName::accept_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("why");

    QTest::newRow("A-1-1 root")         << QStringLiteral("root")              << QStringLiteral("canonical default config");
    QTest::newRow("A-1-2 home")         << QStringLiteral("home")              << QStringLiteral("common second config");
    QTest::newRow("A-1-3 mixed chars")  << QStringLiteral("my-config_01.test") << QStringLiteral("letters/digits/_/-/.");
    QTest::newRow("single char")        << QStringLiteral("a")                 << QStringLiteral("minimal valid");
    QTest::newRow("digits only")        << QStringLiteral("42")                << QStringLiteral("snapper allows numeric");
}

void TestConfigName::accept()
{
    QFETCH(QString, name);
    QFETCH(QString, why);
    QVERIFY2(validateConfigName(name),
             qPrintable(QStringLiteral("expected accept (%1): %2").arg(why, name)));
}

void TestConfigName::reject_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("why");

    QTest::newRow("A-1-4 empty")        << QString()                           << QStringLiteral("empty string");
    QTest::newRow("A-1-5 dotdot")       << QStringLiteral("..")                << QStringLiteral("parent dir marker");
    QTest::newRow("A-1-6 traversal")    << QStringLiteral("../etc")            << QStringLiteral("slash + traversal");
    QTest::newRow("A-1-7 suffix trav")  << QStringLiteral("root/..")           << QStringLiteral("slash appears");
    QTest::newRow("A-1-8 absolute")     << QStringLiteral("/etc/snapper")      << QStringLiteral("absolute path");
    QTest::newRow("A-1-9 space")        << QStringLiteral("config with space") << QStringLiteral("whitespace not allowed");
    QTest::newRow("A-1-10 NUL byte")    << QString::fromUtf8("config\0inject", 13) << QStringLiteral("embedded NUL");
    QTest::newRow("A-1-11 non-ASCII")   << QStringLiteral("日本語")             << QStringLiteral("multibyte");
    QTest::newRow("A-1-13 leading -")   << QStringLiteral("-leading-dash")     << QStringLiteral("CLI option lookalike");
    QTest::newRow("single dot")         << QStringLiteral(".")                 << QStringLiteral("current dir marker");
    QTest::newRow("backslash")          << QStringLiteral("root\\other")       << QStringLiteral("backslash not in allowlist");
    QTest::newRow("newline")            << QStringLiteral("root\ninject")      << QStringLiteral("newline");
    QTest::newRow("semicolon")          << QStringLiteral("root;evil")         << QStringLiteral("shell metachar");

    // Round 2 review: 制御文字 (C0 + DEL + C1) 全範囲を allowlist 正規表現 + containsDangerousChar 拡張で拒否
    QTest::newRow("ctrl SOH (0x01)")    << (QStringLiteral("ro") + QChar(0x01) + QStringLiteral("ot"))  << QStringLiteral("C0 control low");
    QTest::newRow("ctrl TAB (0x09)")    << (QStringLiteral("ro") + QChar(0x09) + QStringLiteral("ot"))  << QStringLiteral("HT");
    QTest::newRow("ctrl ESC (0x1B)")    << (QStringLiteral("ro") + QChar(0x1B) + QStringLiteral("ot"))  << QStringLiteral("ESC sequence prefix");
    QTest::newRow("ctrl FS (0x1F)")     << (QStringLiteral("ro") + QChar(0x1F) + QStringLiteral("ot"))  << QStringLiteral("C0 boundary high");
    QTest::newRow("DEL (0x7F)")         << (QStringLiteral("ro") + QChar(0x7F) + QStringLiteral("ot"))  << QStringLiteral("DEL char");
    QTest::newRow("C1 PAD (0x80)")      << (QStringLiteral("ro") + QChar(0x80) + QStringLiteral("ot"))  << QStringLiteral("C1 boundary low");
    QTest::newRow("C1 APC (0x9F)")      << (QStringLiteral("ro") + QChar(0x9F) + QStringLiteral("ot"))  << QStringLiteral("C1 boundary high");
}

void TestConfigName::reject()
{
    QFETCH(QString, name);
    QFETCH(QString, why);
    QVERIFY2(!validateConfigName(name),
             qPrintable(QStringLiteral("expected reject (%1): %2").arg(why, name)));
}

void TestConfigName::lengthBoundary()
{
    // A-1-12 長さ上限 (NAME_MAX = 255 を仮定)
    const QString ok255  = QString(255, QLatin1Char('a'));
    const QString bad256 = QString(256, QLatin1Char('a'));
    QVERIFY2(validateConfigName(ok255),  "255 chars of [a-z] should be accepted (NAME_MAX)");
    QVERIFY2(!validateConfigName(bad256), "256 chars should be rejected");
}

QTEST_APPLESS_MAIN(TestConfigName)
#include "tst_configname.moc"
