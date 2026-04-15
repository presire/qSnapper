// tst_configname.cpp
// レイヤA-1: validateConfigName() の単体テスト (テスト計画 §2 A-1)
//
// 前提となる実装インタフェース (P0-3 で src/dbusservice 配下に新設予定):
//
//   // include/dbusservice/inputvalidator.h
//   namespace qsnapper::security {
//       // configName が安全か判定する。
//       // 仕様:
//       //   - 許容文字: [A-Za-z0-9_.-]
//       //   - 空文字列は拒否
//       //   - 先頭が '-' は拒否 (snapperのCLIオプション誤解釈防止)
//       //   - '/' '..' (完全一致) は明示的に拒否される (正規表現で自然にはじかれるが、
//       //     可読性のため個別チェックも推奨)
//       //   - NUL混入は拒否
//       //   - 長さ上限 255 (NAME_MAX準拠)
//       bool validateConfigName(const QString &name);
//   }
//
// テスト計画 §2 A-1 の13ケースに対応。

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
