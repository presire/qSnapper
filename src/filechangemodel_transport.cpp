/**
 * @file filechangemodel_transport.cpp
 * @brief 復元計画D-Bus転送の破棄処理とシステムバス実装
 *
 * staged restoreのD-Busメソッドを非同期で発行し、進捗と完了のシグナルを中継する
 * Polkit認証はcommitPlanの呼び出し時にqsnapper-dbus-serviceで1度だけ行う
 *
 * 利用する側 : FileChangeModel
 * 依存 : Qt D-Bus、filechangemodel_transport_p.h
 */

#include "filechangemodel_transport_p.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
    /**
     * @brief 復元計画のD-Busエラーを、利用者へ表示できる文言へ変換する
     *
     * BeginRestorePlanなどはPolkitの認可に失敗するとAccessDeniedを返すため、qsnapper-dbus-serviceが返す生のメッセージではなく読みやすい説明にする
     * それ以外のエラーはqsnapper-dbus-serviceが返した文言をそのまま使う
     *
     * @param error D-Busの応答に含まれるエラー
     * @return 画面に表示する文言
     */
    QString planErrorText(const QDBusError &error)
    {
        const QString name = error.name();
        if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
                || name == QLatin1String("com.presire.qsnapper.Error.AccessDenied")
                || name == QLatin1String("com.presire.qsnapper.Error.NotAuthorized")) {
            return QCoreApplication::translate("FileChangeModel",
                "Authorization was denied. Administrator authentication is required to restore files.");
        }
        return error.message();
    }
} // namespace


// ============================================================================
// RestorePlanTransport / SystemBusRestorePlanTransport Implementation
// ============================================================================

RestorePlanTransport::~RestorePlanTransport() = default;

/**
 * @brief SystemBusRestorePlanTransportを構築する
 *
 * @param parent 親QObject (未指定の場合は呼び出し側がdeleteする)
 */
SystemBusRestorePlanTransport::SystemBusRestorePlanTransport(QObject *parent)
    : QObject(parent)
{
}

/**
 * @brief staging計画の作成を要求する
 *
 * @param configName Snapper設定名
 * @param snapshotNumber スナップショット番号
 * @param counterpartSnapshotNumber 比較相手のスナップショット番号
 * @param restoreMode 復元方式 ("yast" または "direct")
 * @param done 結果コールバック (ok, マニフェストID, error)
 */
void SystemBusRestorePlanTransport::beginPlan(const QString &configName, int snapshotNumber,
                                              int counterpartSnapshotNumber, const QString &restoreMode,
                                              std::function<void(bool ok, const QString &manifestId, const QString &error)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "BeginRestorePlan"
    );
    message << configName << snapshotNumber << counterpartSnapshotNumber << restoreMode;

    callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            done(false, QString(), planErrorText(reply.error()));
            return;
        }
        // 空のマニフェストIDはサーバ側での計画作成失敗を示す
        const QString manifestId = reply.value();
        done(!manifestId.isEmpty(), manifestId,
             manifestId.isEmpty() ? QStringLiteral("Restore plan was rejected") : QString());
    });
}

/**
 * @brief staging計画へ検証済みエントリのチャンクを追加する
 *
 * @param manifestId 対象計画のマニフェストID
 * @param paths エントリのパスリスト
 * @param changeTypes パスに対応する変更タイプリスト
 * @param done 結果コールバック (ok, error)
 */
void SystemBusRestorePlanTransport::stageEntries(const QString &manifestId, const QStringList &paths, const QStringList &changeTypes,
                                                 std::function<void(bool ok, const QString &error)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "StageRestoreEntries"
    );
    message << manifestId << paths << changeTypes;

    callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            done(false, planErrorText(reply.error()));
        }
        else if (!reply.value()) {
            done(false, QStringLiteral("Restore entries were rejected"));
        }
        else {
            done(true, QString());
        }
    });
}

/**
 * @brief 計画を凍結して認証と実行を開始させる
 *
 * @param manifestId 対象計画のマニフェストID
 * @param done 結果コールバック (ok, error)
 */
void SystemBusRestorePlanTransport::commitPlan(const QString &manifestId,
                                               std::function<void(bool ok, const QString &error)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "CommitRestorePlan"
    );
    message << manifestId;

    callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            done(false, planErrorText(reply.error()));
        }
        else if (!reply.value()) {
            done(false, QStringLiteral("Restore plan commit was rejected"));
        }
        else {
            done(true, QString());
        }
    });
}

/**
 * @brief 計画のキャンセルを要求する
 *
 * @param manifestId 対象計画のマニフェストID
 * @param done 結果コールバック (ok, error)
 */
void SystemBusRestorePlanTransport::cancelPlan(const QString &manifestId,
                                               std::function<void(bool ok, const QString &error)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "CancelRestorePlan"
    );
    message << manifestId;

    callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            done(false, planErrorText(reply.error()));
        }
        else if (!reply.value()) {
            done(false, QStringLiteral("Restore plan cancel was rejected"));
        }
        else {
            done(true, QString());
        }
    });
}

/**
 * @brief 計画の状態をCSV形式で問い合わせる
 *
 * @param manifestId 対象計画のマニフェストID
 * @param done 結果コールバック (ok, statusCsv, error)
 */
void SystemBusRestorePlanTransport::requestStatus(const QString &manifestId,
                                                  std::function<void(bool ok, const QString &statusCsv, const QString &error)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "GetRestorePlanStatus"
    );
    message << manifestId;

    callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            done(false, QString(), planErrorText(reply.error()));
            return;
        }
        // 空の返り値はサーバ側での問い合わせ失敗を示す
        const QString statusCsv = reply.value();
        done(!statusCsv.isEmpty(), statusCsv,
             statusCsv.isEmpty() ? QStringLiteral("Restore plan status was rejected") : QString());
    });
}

/**
 * @brief 復元計画の進捗/完了シグナルとサービス消失通知を受信側に登録する
 *
 * @param receiver 復元計画スロットとonRestorePlanServiceVanishedスロットを持つQObject
 * @return すべてのシグナル登録に成功した場合はtrue
 */
bool SystemBusRestorePlanTransport::subscribePlanSignals(QObject *receiver)
{
    const bool progressOk = QDBusConnection::systemBus().connect(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "restorePlanProgress",
        receiver,
        SLOT(onRestorePlanProgress(QString,int,int))
    );
    const bool finishedOk = QDBusConnection::systemBus().connect(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "restorePlanFinished",
        receiver,
        SLOT(onRestorePlanFinished(QString,QString,QString))
    );
    if (!m_serviceWatcher) {
        m_serviceWatcher = new QDBusServiceWatcher(
            QStringLiteral("com.presire.qsnapper.Operations"),
            QDBusConnection::systemBus(),
            QDBusServiceWatcher::WatchForUnregistration,
            this
        );
    }
    disconnect(m_serviceWatcher, SIGNAL(serviceUnregistered(QString)),
               receiver, SLOT(onRestorePlanServiceVanished()));
    const bool watcherOk = connect(
        m_serviceWatcher,
        SIGNAL(serviceUnregistered(QString)),
        receiver,
        SLOT(onRestorePlanServiceVanished())
    );
    return progressOk && finishedOk && watcherOk;
}

/**
 * @brief 復元計画の進捗/完了シグナルとサービス消失通知の受信を解除する
 *
 * @param receiver 登録解除するQObject
 */
void SystemBusRestorePlanTransport::unsubscribePlanSignals(QObject *receiver)
{
    QDBusConnection::systemBus().disconnect(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "restorePlanProgress",
        receiver,
        SLOT(onRestorePlanProgress(QString,int,int))
    );
    QDBusConnection::systemBus().disconnect(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "restorePlanFinished",
        receiver,
        SLOT(onRestorePlanFinished(QString,QString,QString))
    );
    if (m_serviceWatcher) {
        disconnect(m_serviceWatcher, SIGNAL(serviceUnregistered(QString)),
                   receiver, SLOT(onRestorePlanServiceVanished()));
    }
}

/**
 * @brief 復元計画メソッドを非同期で呼び出す (タイムアウトなし)
 *
 * watcherはthisの子として管理されるため、transportの破棄時に未完了のコールバックが発火することはない
 *
 * @param message 送信するメソッドコール
 * @param handler watcherの完了時に呼び出すハンドラ
 */
void SystemBusRestorePlanTransport::callPlanMethod(const QDBusMessage &message,
                                                   std::function<void(QDBusPendingCallWatcher *watcher)> handler)
{
    QDBusPendingCall pendingCall = QDBusConnection::systemBus().asyncCall(message, -1);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [handler, watcher](QDBusPendingCallWatcher *w) {
        Q_UNUSED(w);
        handler(watcher);
        watcher->deleteLater();
    });
}
