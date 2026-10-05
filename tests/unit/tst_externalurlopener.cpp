// tst_externalurlopener.cpp
//
// externalurlopenerの単体テスト
// 外部ブラウザで開くURLを、httpとhttpsだけに限定できることを確認する

#include <QtTest/QtTest>
#include <QDBusError>
#include <QDBusMessage>
#include <QUrl>

#include "externalurlopener.h"

/**
 * @brief externalurlopenerの単体テスト
 */
class TestExternalUrlOpener : public QObject
{
    Q_OBJECT

private slots:
    void allowedUrl_data();
    void allowedUrl();
    void rejectedUrl_data();
    void rejectedUrl();
    void disallowedUrlIsNotOpened();
    void fallbackDecision_data();
    void fallbackDecision();
};

/**
 * @brief 許可するURLのテストデータを用意する
 */
void TestExternalUrlOpener::allowedUrl_data()
{
    QTest::addColumn<QString>("url");

    QTest::newRow("https") << QStringLiteral("https://github.com/presire/qSnapper");
    QTest::newRow("http") << QStringLiteral("http://www.qt.io/");
    QTest::newRow("uppercase scheme") << QStringLiteral("HTTPS://www.qt.io/licensing/");
    QTest::newRow("with port and query") << QStringLiteral("https://example.org:8443/a?b=c#d");
    QTest::newRow("host only") << QStringLiteral("https://github.com");
    QTest::newRow("ipv6 host") << QStringLiteral("https://[::1]:8080/");
}

/**
 * @brief httpとhttpsのURLが許可されることを確認する
 */
void TestExternalUrlOpener::allowedUrl()
{
    QFETCH(QString, url);
    QVERIFY(qsnapper::desktop::isAllowedExternalUrl(QUrl(url, QUrl::StrictMode)));
}

/**
 * @brief 拒否するURLのテストデータを用意する
 */
void TestExternalUrlOpener::rejectedUrl_data()
{
    QTest::addColumn<QString>("url");

    QTest::newRow("empty") << QString();
    QTest::newRow("file") << QStringLiteral("file:///etc/passwd");
    QTest::newRow("file with host") << QStringLiteral("file://localhost/etc/passwd");
    QTest::newRow("ftp") << QStringLiteral("ftp://example.org/a");
    QTest::newRow("mailto") << QStringLiteral("mailto:root@example.org");
    QTest::newRow("desktop file") << QStringLiteral("applications:org.kde.konsole.desktop");
    QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)");
    QTest::newRow("no host") << QStringLiteral("https:///path");
    QTest::newRow("relative") << QStringLiteral("/usr/bin/konsole");
    QTest::newRow("no scheme") << QStringLiteral("example.org/a");
    QTest::newRow("space in host") << QStringLiteral("https://exa mple.org/");
    QTest::newRow("control character in host") << QStringLiteral("https://exa\nmple.org/");
    QTest::newRow("invalid percent escape") << QStringLiteral("https://example.org/%zz");
}

/**
 * @brief httpとhttps以外、およびホストの無いURLが拒否されることを確認する
 */
void TestExternalUrlOpener::rejectedUrl()
{
    QFETCH(QString, url);
    QVERIFY(!qsnapper::desktop::isAllowedExternalUrl(QUrl(url, QUrl::StrictMode)));
}

/**
 * @brief 許可されないURLでは、何も開かずにfalseを返すことを確認する
 */
void TestExternalUrlOpener::disallowedUrlIsNotOpened()
{
    QObject context;
    QVERIFY(!qsnapper::desktop::openExternalUrl(QUrl(QStringLiteral("file:///etc/passwd")), &context));
}

/**
 * @brief フォールバック判定のテストデータを用意する
 */
void TestExternalUrlOpener::fallbackDecision_data()
{
    QTest::addColumn<QString>("errorName");
    QTest::addColumn<bool>("fallback");

    QTest::newRow("service unknown") << QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown") << true;
    QTest::newRow("name has no owner") << QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner") << true;
    QTest::newRow("spawn service not found") << QStringLiteral("org.freedesktop.DBus.Error.Spawn.ServiceNotFound") << true;
    QTest::newRow("spawn exec failed") << QStringLiteral("org.freedesktop.DBus.Error.Spawn.ExecFailed") << true;
    QTest::newRow("unknown method") << QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod") << true;
    QTest::newRow("unknown interface") << QStringLiteral("org.freedesktop.DBus.Error.UnknownInterface") << true;
    QTest::newRow("unknown object") << QStringLiteral("org.freedesktop.DBus.Error.UnknownObject") << true;
    QTest::newRow("no server") << QStringLiteral("org.freedesktop.DBus.Error.NoServer") << true;
    QTest::newRow("access denied") << QStringLiteral("org.freedesktop.DBus.Error.AccessDenied") << false;
    QTest::newRow("portal not allowed") << QStringLiteral("org.freedesktop.portal.Error.NotAllowed") << false;
    QTest::newRow("portal failed") << QStringLiteral("org.freedesktop.portal.Error.Failed") << false;
    QTest::newRow("no reply") << QStringLiteral("org.freedesktop.DBus.Error.NoReply") << false;
    QTest::newRow("timeout") << QStringLiteral("org.freedesktop.DBus.Error.Timeout") << false;
    QTest::newRow("invalid args") << QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs") << false;
    QTest::newRow("other spawn-like name") << QStringLiteral("org.example.Spawn.Failed") << false;
}

/**
 * @brief ポータルが無いと分かるエラーだけが、直接起動の対象になることを確認する
 */
void TestExternalUrlOpener::fallbackDecision()
{
    QFETCH(QString, errorName);
    QFETCH(bool, fallback);

    const QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.OpenURI"),
        QStringLiteral("OpenURI"));
    const QDBusError error(call.createErrorReply(errorName, QStringLiteral("test")));
    QVERIFY(error.isValid());
    QCOMPARE(qsnapper::desktop::shouldFallBackToDirectLaunch(error), fallback);
}

QTEST_GUILESS_MAIN(TestExternalUrlOpener)
#include "tst_externalurlopener.moc"
