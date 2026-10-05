#ifndef EXTERNALURLOPENER_H
#define EXTERNALURLOPENER_H

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
#include <QDebug>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

namespace qsnapper::desktop {

/**
 * @brief 外部ブラウザで開いてよいURLか判定する
 *
 * httpとhttpsだけを許可する
 * ホスト名を持たないURLおよび不正なURLは拒否する
 * リンクの文字列はQMLから渡されるため、file:やデスクトップ固有のスキームで任意のアプリケーションは起動不可とする
 *
 * @param url 判定するURL
 * @return 開いてよい場合はtrue
 */
inline bool isAllowedExternalUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

/**
 * @brief ポータルの呼び出し失敗時に、直接起動へ切り替えてよいか判定する
 *
 * ポータルが存在せず、要求が処理されなかったことが明らかなエラーだけを対象にする
 * - サービスが無い、または、D-Busから起動できない (ServiceUnknown, NameHasNoOwner, Spawn.*)
 * - インターフェース / メソッドが無い (UnknownObject, UnknownInterface, UnknownMethod)
 * - セッションバスに接続できない (NoServer, Disconnected)
 *
 * ポータルが明示的に拒否した場合 (AccessDeniedやNotAllowed等) は、拒否を迂回しないために切り替えない
 * 応答のタイムアウト (NoReply) も、要求が処理済みの可能性があり2重に開いてしまうため切り替えない
 *
 * @param error ポータル呼び出しのエラー
 * @return 直接起動へ切り替えてよい場合はtrue
 */
inline bool shouldFallBackToDirectLaunch(const QDBusError &error)
{
    switch (error.type()) {
    case QDBusError::ServiceUnknown:
    case QDBusError::UnknownObject:
    case QDBusError::UnknownInterface:
    case QDBusError::UnknownMethod:
    case QDBusError::NoServer:
    case QDBusError::Disconnected:
        return true;
    default:
        break;
    }
    const QString name = error.name();
    return name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || name.startsWith(QLatin1String("org.freedesktop.DBus.Error.Spawn."));
}

/**
 * @brief URLを外部ブラウザで開く
 *
 * まずsession busのデスクトップポータル (org.freedesktop.portal.OpenURI) に依頼する
 * ブラウザはポータルを動かしているユーザーセッション側のプロセスとして起動されるため、SELinuxで制限されたqsnapper_tの権限に縛られない
 *
 * ポータルが存在しない場合だけ、従来どおりQDesktopServicesで開く (shouldFallBackToDirectLaunch()を参照)
 * この場合、ブラウザはqsnapper_tから直接起動される
 *
 * 呼び出しは非同期で、フォールバックはcontextのイベントループ上で行う
 * URLはログへ出力しない
 *
 * @param url 開くURL (isAllowedExternalUrl()を満たすもの)
 * @param context 応答を受け取るオブジェクト。このオブジェクトが先に破棄された場合、フォールバックは行われない
 * @return 開く処理を開始できた場合はtrue
 *         URLが許可されない場合はfalse
 */
inline bool openExternalUrl(const QUrl &url, QObject *context)
{
    if (!isAllowedExternalUrl(url)) {
        return false;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || context == nullptr) {
        return QDesktopServices::openUrl(url);
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.OpenURI"),
        QStringLiteral("OpenURI"));
    // 引数: 親ウィンドウの識別子 (なし), URI, オプション (なし)
    message << QString() << url.toString(QUrl::FullyEncoded) << QVariantMap();

    constexpr int PortalTimeoutMs = 5000;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, PortalTimeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [url](QDBusPendingCallWatcher *finished) {
                         if (finished->isError()) {
                             if (shouldFallBackToDirectLaunch(finished->error())) {
                                 QDesktopServices::openUrl(url);
                             }
                             else {
                                 qWarning() << "Desktop portal did not open the link";
                             }
                         }
                         finished->deleteLater();
                     });
    return true;
}

}  // namespace qsnapper::desktop

#endif  // EXTERNALURLOPENER_H
