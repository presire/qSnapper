/**
 * @file snapshotoperations_settings.cpp
 * @brief Settings画面が使う設定の読み取りと、snapperのsystemd timerの管理
 *
 * GetConfig、GetTimerStates、SetTimerEnabledを扱う
 * 全ての公開スロットは、専用のpolkitアクションで共通ゲート (authorizeThen) を通してから本体を実行する
 *
 * timerの操作はsystemdの応答を待つため、本体から戻った後に非同期で応答する
 * 本サービスは単一スレッドで動作し、同期呼び出しで待つと他の要求が全て止まるため
 * 非同期にする代わりに、変更を伴うsystemdの呼び出しの直前ごとに、復元の実行中でないことと呼び出し元が接続していることを確かめ直す
 */

#include <QDebug>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <memory>
#include "snapshotoperations.h"
#include "inputvalidator.h"
#include <snapper/Snapper.h>
#include <snapper/Exception.h>

using qsnapper::dbusservice::SystemdTimerOperation;

namespace {
    const QString ReadOnlyConfigKeys[] = {
        QStringLiteral("SUBVOLUME"),    // 設定の対象のサブボリューム (表示とRollbackの可否の判定に使う)
        QStringLiteral("FSTYPE"),       // ファイルシステムの種類 (表示とRollbackの可否の判定に使う)
        QStringLiteral("QGROUP"),       // quotaのqgroup (表示に使う)
    };

    const QString TimerQueryFailedText = QStringLiteral("Failed to query timer state");             // 状態の取得の失敗
    const QString TimerChangeFailedText = QStringLiteral("Failed to change the timer state");       // 有効化と無効化の失敗
    const QString TimerBusyText = QStringLiteral("Another timer operation is in progress");         // 同時実行の拒否
    const QString InvalidTimerUnitText = QStringLiteral("Invalid timer unit");                      // 許可リスト外のunit名

    // authorizeThenのテンプレート引数に入れ子の山括弧を書くと、認可境界の静的検査 (check_restore_authorization_boundaries.py) が呼び出しを検出できないため別名にする
    using ConfigSettings = QMap<QString, QString>;
}

/**
 * @brief Snapper設定の値を読み取る
 *
 * 共有のSnapperインスタンスは使わず、設定ファイルだけを読む静的なAPIで読み取る
 * ただし、復元の実行中でも読めるようにする理由が無いため、共通ゲートの既定 (復元の実行中は拒否) のままにする
 *
 * @param configName Snapper設定名 (空文字列は"root"として扱う)
 * @return 設定のキーと値のマップ
 */
QMap<QString, QString> SnapshotOperations::GetConfig(const QString &configName)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return {};
    }

    return authorizeThen<ConfigSettings>(
        QStringLiteral("com.presire.qsnapper.view-config"),
        [this, config = *cfg]() {
            return getConfigAuthorized(config);
        });
}

/**
 * @brief 認可済みのGetConfig本体
 *
 * 返すキーは、書き込みを許可する18キーと、読み取り専用の3キーに限る
 * 設定ファイルにある他のキー (ALLOW_USERSなど) は、Settings画面で扱わないため返さない
 * 値に制御文字が含まれる場合は、改ざんされた設定ファイルとみなし、一部だけを返さずに失敗とする
 *
 * @param configName 検証済みSnapper設定名
 * @return 設定のキーと値のマップ、失敗時は空のマップ (エラー応答は送出済み)
 */
QMap<QString, QString> SnapshotOperations::getConfigAuthorized(const QString &configName)
{
    if (!qsnapper::security::validateConfigName(configName)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid configName"));
        return {};
    }

    try {
        const snapper::ConfigInfo info = snapper::Snapper::getConfig(configName.toStdString(), "/");

        QStringList keys = qsnapper::security::editableSnapperConfigKeys();
        for (const QString &key : ReadOnlyConfigKeys) {
            keys.append(key);
        }

        QMap<QString, QString> values;
        for (const QString &key : keys) {
            std::string value;
            if (!info.get_value(key.toStdString(), value)) {
                continue;
            }
            const QString text = QString::fromStdString(value);
            if (!qsnapper::security::isRecordSafeText(text)) {
                qWarning() << "Snapper config contains an unsafe value";
                replyError(QDBusError::Failed, QStringLiteral("Failed to read config"));
                return {};
            }
            values.insert(key, text);
        }
        return values;
    }
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Failed to read snapper config";
    }
    catch (const std::exception &) {
        qWarning() << "Failed to read snapper config";
    }
    replyError(QDBusError::Failed, QStringLiteral("Failed to read config"));
    return {};
}

/**
 * @brief snapperのsystemd timerの状態を読み取る
 *
 * timerは全てのSnapper設定に共通であり、config名を受け取らない
 * 状態の取得もsystemdの応答を待つため、同時に1件だけに限り、他の要求を待たせない
 *
 * @return 応答は非同期に送るため、戻り値は使われない
 */
SnapperTimerStateMap SnapshotOperations::GetTimerStates()
{
    if (rejectWhileTimerOperationRunning()) {
        return {};
    }

    return authorizeThen<SnapperTimerStateMap>(
        QStringLiteral("com.presire.qsnapper.view-timers"),
        [this]() {
            return getTimerStatesAuthorized();
        });
}

/**
 * @brief 認可済みのGetTimerStates本体
 *
 * 認可を待つ間に別のtimerの操作が始まり得るため、本体でも同時実行を確かめ直す
 *
 * @return 常に空のマップ (応答は操作の完了時に送る)
 */
SnapperTimerStateMap SnapshotOperations::getTimerStatesAuthorized()
{
    if (rejectWhileTimerOperationRunning()) {
        return {};
    }

    const auto reply = claimReplyForAsyncCompletion();
    if (!reply) {
        return {};
    }

    auto *operation = new SystemdTimerOperation(reply->connection, SystemdTimerOperation::Kind::Query,
                                                qsnapper::security::snapperTimerUnits(), {}, this);
    m_timerOperation = operation;
    resetIdleTimer();

    operation->start([this, operation, reply = *reply](SystemdTimerOperation::Outcome outcome,
                                                       const SnapperTimerStateMap &states) {
        operation->deleteLater();
        m_timerOperation.clear();
        resetIdleTimer();

        if (outcome != SystemdTimerOperation::Outcome::Succeeded) {
            sendAsyncError(reply, QDBusError::errorString(QDBusError::Failed), TimerQueryFailedText);
            return;
        }
        reply.connection.send(reply.message.createReply(QVariant::fromValue(states)));
    });
    return {};
}

/**
 * @brief snapperのsystemd timerを有効化して起動する、または停止して無効化する
 *
 * unit名は、認可の前と本体の直前の両方で、固定の許可リストとの完全一致で確かめる
 * systemdへ渡すのは許可リストの定数であり、呼び出し元の文字列は渡さない
 *
 * @param unit unit名
 * @param enabled trueで有効化と起動、falseで停止と無効化
 * @return 応答は非同期に送るため、戻り値は使われない
 */
bool SnapshotOperations::SetTimerEnabled(const QString &unit, bool enabled)
{
    if (!qsnapper::security::canonicalSnapperTimerUnit(unit)) {
        replyError(QDBusError::InvalidArgs, InvalidTimerUnitText);
        return false;
    }
    if (rejectWhileTimerOperationRunning()) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.manage-timers"),
        [this, unit, enabled]() {
            return setTimerEnabledAuthorized(unit, enabled);
        });
}

/**
 * @brief 認可済みのSetTimerEnabled本体
 *
 * 変更を伴うsystemdの呼び出しの直前ごとに、次の両方を満たすことを確かめる
 *   - 復元の実行中でない (復元はシステムの状態を前提に進むため、その間にサービスの構成を変えない)
 *   - 呼び出し元がバスに接続している (結果を受け取れない呼び出し元のために、変更を続けない)
 * 満たさなくなった時点で新たな変更を止め、実際の状態を読み直してから失敗を返す
 *
 * @param unit 呼び出し元が指定したunit名
 * @param enabled trueで有効化と起動、falseで停止と無効化
 * @return 常にfalse (応答は操作の完了時に送る)
 */
bool SnapshotOperations::setTimerEnabledAuthorized(const QString &unit, bool enabled)
{
    const auto canonical = qsnapper::security::canonicalSnapperTimerUnit(unit);
    if (!canonical) {
        replyError(QDBusError::InvalidArgs, InvalidTimerUnitText);
        return false;
    }
    if (rejectWhileTimerOperationRunning()) {
        return false;
    }

    const auto reply = claimReplyForAsyncCompletion();
    if (!reply) {
        return false;
    }

    // 呼び出し元の切断は、監視を始めてから現在の接続の有無を確かめることで、見落としなく検出する
    // 先に確かめてから監視を始めると、その間に切断された場合を見落とすため
    const QString caller = reply->message.service();
    auto callerGone = std::make_shared<bool>(false);
    auto *callerWatcher = new QDBusServiceWatcher(caller, reply->connection,
                                                  QDBusServiceWatcher::WatchForUnregistration, this);
    connect(callerWatcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [callerGone](const QString &) { *callerGone = true; });
    QDBusConnectionInterface *busInterface = reply->connection.interface();
    if (!busInterface || !busInterface->isServiceRegistered(caller).value()) {
        *callerGone = true;
    }

    // 変更を止めた理由は、止めた時点で記録する
    // 止めた後の読み直しの間に復元が終わると、完了時に判定し直しても止めた理由が分からなくなるため
    auto stoppedByRestore = std::make_shared<bool>(false);
    auto mutationGuard = [this, callerGone, stoppedByRestore]() {
        if (!m_restoreExecutions.isEmpty()) {
            *stoppedByRestore = true;
            return false;
        }
        return !*callerGone;
    };

    const auto kind = enabled ? SystemdTimerOperation::Kind::Enable : SystemdTimerOperation::Kind::Disable;
    auto *operation = new SystemdTimerOperation(reply->connection, kind, QStringList{*canonical},
                                                mutationGuard, this);
    m_timerOperation = operation;
    resetIdleTimer();

    operation->start([this, operation, callerWatcher, stoppedByRestore,
                      reply = *reply](SystemdTimerOperation::Outcome outcome, const SnapperTimerStateMap &) {
        operation->deleteLater();
        callerWatcher->deleteLater();
        m_timerOperation.clear();
        resetIdleTimer();

        if (outcome == SystemdTimerOperation::Outcome::Succeeded) {
            reply.connection.send(reply.message.createReply(QVariant::fromValue(true)));
            return;
        }
        if (outcome == SystemdTimerOperation::Outcome::Interrupted && *stoppedByRestore) {
            sendAsyncError(reply, QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_NAME),
                           QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_TEXT));
            return;
        }
        sendAsyncError(reply, QDBusError::errorString(QDBusError::Failed), TimerChangeFailedText);
    });
    return false;
}

/**
 * @brief 現在の呼び出しの応答を、非同期に送るために確保する
 *
 * 遅延許可の経路 (completeDeferredCall) では、確保した時点で送出済みとして扱わせ、本体の戻り値を応答として送らせない
 * 即時許可の経路では、QDBusContextの遅延応答に切り替え、スロットの戻り値を応答として送らせない
 *
 * @return 確保した応答コンテキスト
 */
std::optional<SnapshotOperations::CallReply> SnapshotOperations::claimReplyForAsyncCompletion()
{
    if (m_deferredReply) {
        if (m_deferredReply->replied || !m_deferredReply->fromDBus) {
            return std::nullopt;
        }
        CallReply reply = *m_deferredReply;
        m_deferredReply->replied = true;
        return reply;
    }

    CallReply reply = captureCallReply();
    if (!reply.fromDBus) {
        return std::nullopt;
    }
    setDelayedReply(true);
    return reply;
}

/**
 * @brief 確保した応答コンテキストへエラーを送る
 *
 * 呼び出しのcontextが失われた後に送るため、replyError()ではなくキャプチャ済みのmessageから応答を組み立てる
 */
void SnapshotOperations::sendAsyncError(const CallReply &reply, const QString &errorName, const QString &text)
{
    if (!reply.fromDBus) {
        return;
    }
    reply.connection.send(reply.message.createErrorReply(errorName, text));
}

/**
 * @brief 実行中のtimerの操作があれば、固定の文言で拒否する
 *
 * 待ち行列を設けずに拒否するのは、systemdが応答しない場合に要求が際限なく溜まるのを防ぐため
 */
bool SnapshotOperations::rejectWhileTimerOperationRunning()
{
    if (!m_timerOperation) {
        return false;
    }
    replyError(QDBusError::LimitsExceeded, TimerBusyText);
    return true;
}

/**
 * @brief timerの有効化または無効化の実行中かを判定する
 *
 * 操作の完了 (finish) まではm_timerOperationが残るため、jobの完了を待つ間も実行中として扱う
 */
bool SnapshotOperations::timerChangeInProgress() const
{
    // snapperdへ転送した変更も、送信後は取り消せないため、復元の開始と重ねない
    return (m_timerOperation && m_timerOperation->mutatesUnits()) || snapperdOperationRunning();
}
