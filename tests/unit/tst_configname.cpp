/**
 * @file tst_configname.cpp
 * @brief validateConfigName() と validateSnapperConfigSettings() の単体テスト
 *
 * 契約の詳細は src/dbusservice/inputvalidator.h を参照する
 * snapper 設定名と WriteSnapperConfig の設定マップが、危険な入力を
 * 正しく受け入れ・拒否することを確認する
 */

#include <QtTest/QtTest>
#include <QSet>
#include <QString>
#include <QStringList>

#include "inputvalidator.h"

using qsnapper::security::canonicalSnapperTimerUnit;
using qsnapper::security::editableSnapperConfigKeys;
using qsnapper::security::snapperTimerUnits;
using qsnapper::security::validateConfigName;
using qsnapper::security::validateSnapperConfigSettings;

/** @brief WriteSnapperConfig で送受信する設定キーと値のマップ */
using SettingsMap = QMap<QString, QString>;

/**
 * @class TestConfigName
 * @brief 設定名と設定マップの検証関数の境界条件を確認するテストスイート
 */
class TestConfigName : public QObject
{
    Q_OBJECT

private slots:
    // 受理されるべき
    /** @brief 設定名として受け入れるべき入力のデータ行を定義する */
    void accept_data();
    /** @brief 設定名が受け入れられることを確認する */
    void accept();

    // 拒否されるべき
    /** @brief 設定名として拒否すべき入力のデータ行を定義する */
    void reject_data();
    /** @brief 設定名が拒否されることを確認する */
    void reject();

    // 長さ境界値
    /** @brief 設定名の長さ上限 NAME_MAX の境界を確認する */
    void lengthBoundary();

    // WriteSnapperConfigの設定 (受理されるべき)
    /** @brief 設定マップとして受け入れるべき入力のデータ行を定義する */
    void settingsAccept_data();
    /** @brief 設定マップが受け入れられることを確認する */
    void settingsAccept();

    // WriteSnapperConfigの設定 (拒否されるべき)
    /** @brief 設定マップとして拒否すべき入力のデータ行を定義する */
    void settingsReject_data();
    /** @brief 設定マップが拒否されることを確認する */
    void settingsReject();

    // 設定画面が編集する18キー
    /** @brief 編集可能キーの一覧が18個で、すべて検証を通ることを確認する */
    void editableKeysAccepted();
    /** @brief 編集可能キーの値に注入用の文字が含まれる場合に拒否されることを確認する */
    void editableKeysRejectInjection();

    // SetTimerEnabledのユニット名
    /** @brief 管理対象の3つのタイマだけが受け入れられることを確認する */
    void timerUnitAccept();
    /** @brief ユニット名として拒否すべき入力のデータ行を定義する */
    void timerUnitReject_data();
    /** @brief 許可リスト外のユニット名が拒否されることを確認する */
    void timerUnitReject();
};

/**
 * @brief validateConfigName() が受け入れるべき設定名を定義する
 *
 * 既定設定名、一般的な名前、英数字と記号の混在、最小長、数字のみを確認する
 */
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

/**
 * @brief validateConfigName() が正当な設定名を受け入れることを確認する
 */
void TestConfigName::accept()
{
    QFETCH(QString, name);
    QFETCH(QString, why);
    QVERIFY2(validateConfigName(name),
             qPrintable(QStringLiteral("expected accept (%1): %2").arg(why, name)));
}

/**
 * @brief validateConfigName() が拒否すべき設定名を定義する
 *
 * 空文字列、ドットドット、パストラバーサル、絶対パス、空白、NUL バイト
 * 非 ASCII、先頭ダッシュ、改行、シェルメタ文字、制御文字全範囲を含む
 */
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

/**
 * @brief validateConfigName() が不正な設定名を拒否することを確認する
 */
void TestConfigName::reject()
{
    QFETCH(QString, name);
    QFETCH(QString, why);
    QVERIFY2(!validateConfigName(name),
             qPrintable(QStringLiteral("expected reject (%1): %2").arg(why, name)));
}

/**
 * @brief 設定名の長さ上限 NAME_MAX の境界を確認する
 *
 * A-1-12 として 255 文字は受理し、256 文字は拒否することを検証する
 */
void TestConfigName::lengthBoundary()
{
    // A-1-12 長さ上限 (NAME_MAX = 255 を仮定)
    const QString ok255  = QString(255, QLatin1Char('a'));
    const QString bad256 = QString(256, QLatin1Char('a'));
    QVERIFY2(validateConfigName(ok255),  "255 chars of [a-z] should be accepted (NAME_MAX)");
    QVERIFY2(!validateConfigName(bad256), "256 chars should be rejected");
}

/**
 * @brief validateSnapperConfigSettings() が受け入れるべき設定マップを定義する
 *
 * クライアントが実際に送る組み合わせ、単一カウント、同値レンジ、上限カウンタ
 * 秒数ゼロ、小数の範囲と真偽値を確認する
 */
void TestConfigName::settingsAccept_data()
{
    QTest::addColumn<SettingsMap>("settings");

    // 削除前のクライアントが送っていた組み合わせ (互換性の確認として残す)
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

/**
 * @brief validateSnapperConfigSettings() が正当な設定マップを受け入れることを確認する
 */
void TestConfigName::settingsAccept()
{
    QFETCH(SettingsMap, settings);
    QVERIFY(validateSnapperConfigSettings(settings));
}

/**
 * @brief validateSnapperConfigSettings() が拒否すべき設定マップを定義する
 *
 * 空マップ、権限や対象を変えるキー、未知キー、大文字小文字違い、改行注入
 * 不正な真偽値、不正なカウント、不正なレンジ、不正な小数を含む
 */
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

/**
 * @brief validateSnapperConfigSettings() が不正な設定マップを拒否することを確認する
 */
void TestConfigName::settingsReject()
{
    QFETCH(SettingsMap, settings);
    QVERIFY(!validateSnapperConfigSettings(settings));
}

/**
 * @brief 編集可能キーの一覧が18個で、各キーが検証を通ることを確認する
 *
 * 設定画面は一覧の全キーを送り得るため、一覧と検証の許可リストが食い違うと、画面からの保存が必ず失敗する
 * 種別ごとに有効な値を1つずつ与えて確認する
 */
void TestConfigName::editableKeysAccepted()
{
    const QStringList &keys = editableSnapperConfigKeys();
    QCOMPARE(keys.size(), 18);
    QCOMPARE(QSet<QString>(keys.begin(), keys.end()).size(), 18);

    const QStringList validValues = {
        QStringLiteral("yes"), QStringLiteral("0"), QStringLiteral("2-10"), QStringLiteral("0.5"),
    };
    for (const QString &key : keys) {
        bool accepted = false;
        for (const QString &value : validValues) {
            if (validateSnapperConfigSettings(SettingsMap{ { key, value } })) {
                accepted = true;
                break;
            }
        }
        QVERIFY2(accepted, qPrintable(key));
    }

    // 読み取り専用の項目は一覧に含めない (設定画面から書き換えさせないため)
    QVERIFY(!keys.contains(QStringLiteral("SUBVOLUME")));
    QVERIFY(!keys.contains(QStringLiteral("FSTYPE")));
    QVERIFY(!keys.contains(QStringLiteral("QGROUP")));
    QVERIFY(!keys.contains(QStringLiteral("ALLOW_USERS")));
}

/**
 * @brief 編集可能キーの値に改行、NUL、制御文字、引用符が含まれる場合に拒否されることを確認する
 *
 * libsnapperは値を引用符で囲んで書き出すだけでエスケープしないため、これらの文字は別のキーの注入につながる
 */
void TestConfigName::editableKeysRejectInjection()
{
    const QStringList suffixes = {
        QStringLiteral("\n"), QStringLiteral("\r"), QString(QChar(u'\0')), QString(QChar(0x01)),
        QString(QChar(0x7F)), QStringLiteral("\""), QStringLiteral("'"), QStringLiteral(" "),
    };
    for (const QString &key : editableSnapperConfigKeys()) {
        for (const QString &suffix : suffixes) {
            for (const QString &value : { QStringLiteral("yes"), QStringLiteral("0"), QStringLiteral("0.5") }) {
                QVERIFY2(!validateSnapperConfigSettings(SettingsMap{ { key, value + suffix } }), qPrintable(key));
                QVERIFY2(!validateSnapperConfigSettings(SettingsMap{ { key, suffix + value } }), qPrintable(key));
            }
        }
    }
}

/**
 * @brief 管理対象の3つのタイマだけが受け入れられ、許可リスト内の定数が返ることを確認する
 */
void TestConfigName::timerUnitAccept()
{
    const QStringList expected = {
        QStringLiteral("snapper-timeline.timer"),
        QStringLiteral("snapper-cleanup.timer"),
        QStringLiteral("snapper-boot.timer"),
    };
    QCOMPARE(snapperTimerUnits(), expected);
    for (const QString &unit : expected) {
        const std::optional<QString> canonical = canonicalSnapperTimerUnit(unit);
        QVERIFY(canonical.has_value());
        QCOMPARE(*canonical, unit);
    }
}

/**
 * @brief ユニット名として拒否すべき入力を定義する
 *
 * 別のサービス、テンプレートユニット、パス区切り、改行、大文字小文字違い、前後の空白、ユニットのパスを含む
 */
void TestConfigName::timerUnitReject_data()
{
    QTest::addColumn<QString>("unit");

    QTest::newRow("empty")            << QString();
    QTest::newRow("other service")    << QStringLiteral("sshd.service");
    QTest::newRow("other timer")      << QStringLiteral("fstrim.timer");
    QTest::newRow("snapper service")  << QStringLiteral("snapper-timeline.service");
    QTest::newRow("template")         << QStringLiteral("snapper-timeline@root.timer");
    QTest::newRow("absolute path")    << QStringLiteral("/usr/lib/systemd/system/snapper-timeline.timer");
    QTest::newRow("relative path")    << QStringLiteral("../snapper-timeline.timer");
    QTest::newRow("slash")            << QStringLiteral("x/snapper-timeline.timer");
    QTest::newRow("trailing LF")      << QStringLiteral("snapper-timeline.timer\n");
    QTest::newRow("trailing NUL")     << QStringLiteral("snapper-timeline.timer") + QChar(u'\0');
    QTest::newRow("leading space")    << QStringLiteral(" snapper-timeline.timer");
    QTest::newRow("uppercase")        << QStringLiteral("SNAPPER-TIMELINE.TIMER");
    QTest::newRow("no suffix")        << QStringLiteral("snapper-timeline");
    QTest::newRow("glob")             << QStringLiteral("snapper-*.timer");
}

/**
 * @brief 許可リスト外のユニット名が拒否されることを確認する
 */
void TestConfigName::timerUnitReject()
{
    QFETCH(QString, unit);
    QVERIFY(!canonicalSnapperTimerUnit(unit).has_value());
}

QTEST_APPLESS_MAIN(TestConfigName)
#include "tst_configname.moc"
