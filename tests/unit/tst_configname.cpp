// tst_configname.cpp
// validateConfigName() と validateSnapperConfigSettings() の単体テスト。
// 契約の詳細は src/dbusservice/inputvalidator.h を参照。

#include <QtTest/QtTest>
#include <QString>

#include "inputvalidator.h"

using qsnapper::security::validateConfigName;
using qsnapper::security::validateSnapperConfigSettings;

using SettingsMap = QMap<QString, QString>;

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

    // WriteSnapperConfigの設定 (受理されるべき)
    void settingsAccept_data();
    void settingsAccept();

    // WriteSnapperConfigの設定 (拒否されるべき)
    void settingsReject_data();
    void settingsReject();
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
    // '$' は末尾の改行の直前にも一致するため、文字列全体で一致を判定していることを確認する
    QTest::newRow("trailing LF")        << QStringLiteral("root\n")            << QStringLiteral("trailing newline");
    QTest::newRow("trailing CRLF")      << QStringLiteral("root\r\n")          << QStringLiteral("trailing CRLF");
    QTest::newRow("leading LF")         << QStringLiteral("\nroot")            << QStringLiteral("leading newline");
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

void TestConfigName::settingsAccept_data()
{
    QTest::addColumn<SettingsMap>("settings");

    // クライアント (SnapperService::writeSnapperConfig) が実際に送る組み合わせ
    QTest::newRow("client defaults") << SettingsMap{
        { QStringLiteral("NUMBER_CLEANUP"),         QStringLiteral("yes") },
        { QStringLiteral("NUMBER_LIMIT"),           QStringLiteral("2-10") },
        { QStringLiteral("NUMBER_LIMIT_IMPORTANT"), QStringLiteral("4-10") },
        { QStringLiteral("TIMELINE_CREATE"),        QStringLiteral("no") },
    };
    QTest::newRow("single count")    << SettingsMap{ { QStringLiteral("TIMELINE_LIMIT_DAILY"), QStringLiteral("0") } };
    QTest::newRow("equal range")     << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("10-10") } };
    QTest::newRow("max counter")     << SettingsMap{ { QStringLiteral("NUMBER_MIN_AGE"), QStringLiteral("999999999") } };
    QTest::newRow("seconds zero")    << SettingsMap{ { QStringLiteral("EMPTY_PRE_POST_MIN_AGE"), QStringLiteral("0") } };
    QTest::newRow("fraction")        << SettingsMap{ { QStringLiteral("SPACE_LIMIT"), QStringLiteral("0.5") } };
    QTest::newRow("fraction one")    << SettingsMap{ { QStringLiteral("FREE_LIMIT"), QStringLiteral("1.0") } };
    QTest::newRow("bool background") << SettingsMap{ { QStringLiteral("BACKGROUND_COMPARISON"), QStringLiteral("yes") } };
}

void TestConfigName::settingsAccept()
{
    QFETCH(SettingsMap, settings);
    QVERIFY(validateSnapperConfigSettings(settings));
}

void TestConfigName::settingsReject_data()
{
    QTest::addColumn<SettingsMap>("settings");

    QTest::newRow("empty map")          << SettingsMap{};
    // snapperの対象やアクセス権限を変えるキーは本経路から書き込ませない
    QTest::newRow("ALLOW_USERS")        << SettingsMap{ { QStringLiteral("ALLOW_USERS"), QStringLiteral("attacker") } };
    QTest::newRow("ALLOW_GROUPS")       << SettingsMap{ { QStringLiteral("ALLOW_GROUPS"), QStringLiteral("users") } };
    QTest::newRow("SUBVOLUME")          << SettingsMap{ { QStringLiteral("SUBVOLUME"), QStringLiteral("/home") } };
    QTest::newRow("FSTYPE")             << SettingsMap{ { QStringLiteral("FSTYPE"), QStringLiteral("btrfs") } };
    QTest::newRow("SYNC_ACL")           << SettingsMap{ { QStringLiteral("SYNC_ACL"), QStringLiteral("yes") } };
    QTest::newRow("unknown key")        << SettingsMap{ { QStringLiteral("FOO"), QStringLiteral("yes") } };
    QTest::newRow("lowercase key")      << SettingsMap{ { QStringLiteral("number_cleanup"), QStringLiteral("yes") } };
    QTest::newRow("key with LF")        << SettingsMap{ { QStringLiteral("NUMBER_CLEANUP\nALLOW_USERS"), QStringLiteral("yes") } };
    // 1つでも不正なキーがあれば全体を拒否する
    QTest::newRow("valid + invalid")    << SettingsMap{
        { QStringLiteral("NUMBER_CLEANUP"), QStringLiteral("yes") },
        { QStringLiteral("ALLOW_USERS"),    QStringLiteral("attacker") },
    };
    QTest::newRow("bool other word")    << SettingsMap{ { QStringLiteral("NUMBER_CLEANUP"), QStringLiteral("true") } };
    QTest::newRow("bool empty")         << SettingsMap{ { QStringLiteral("NUMBER_CLEANUP"), QString() } };
    QTest::newRow("bool trailing LF")   << SettingsMap{ { QStringLiteral("NUMBER_CLEANUP"), QStringLiteral("yes\n") } };
    QTest::newRow("value injects line") << SettingsMap{ { QStringLiteral("TIMELINE_CREATE"), QStringLiteral("no\nALLOW_USERS=\"x\"") } };
    QTest::newRow("count trailing LF")  << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("10\n") } };
    QTest::newRow("count negative")     << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("-1") } };
    QTest::newRow("count leading zero") << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("010") } };
    QTest::newRow("count too long")     << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("1000000000") } };
    QTest::newRow("count space")        << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral(" 10") } };
    QTest::newRow("range reversed")     << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("10-2") } };
    QTest::newRow("range open")         << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("2-") } };
    QTest::newRow("range three parts")  << SettingsMap{ { QStringLiteral("NUMBER_LIMIT"), QStringLiteral("1-2-3") } };
    QTest::newRow("seconds as range")   << SettingsMap{ { QStringLiteral("NUMBER_MIN_AGE"), QStringLiteral("1-2") } };
    QTest::newRow("seconds decimal")    << SettingsMap{ { QStringLiteral("NUMBER_MIN_AGE"), QStringLiteral("1.5") } };
    QTest::newRow("fraction over one")  << SettingsMap{ { QStringLiteral("SPACE_LIMIT"), QStringLiteral("1.5") } };
    QTest::newRow("fraction size")      << SettingsMap{ { QStringLiteral("SPACE_LIMIT"), QStringLiteral("10GiB") } };
    QTest::newRow("fraction no digits") << SettingsMap{ { QStringLiteral("FREE_LIMIT"), QStringLiteral("0.") } };
    QTest::newRow("fraction ctrl")      << SettingsMap{ { QStringLiteral("FREE_LIMIT"), QStringLiteral("0.2") + QChar(0x1B) } };
}

void TestConfigName::settingsReject()
{
    QFETCH(SettingsMap, settings);
    QVERIFY(!validateSnapperConfigSettings(settings));
}

QTEST_APPLESS_MAIN(TestConfigName)
#include "tst_configname.moc"
