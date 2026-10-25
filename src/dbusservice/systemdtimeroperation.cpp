/**
 * @file systemdtimeroperation.cpp
 * @brief snapperのsystemd timerに対する非同期の操作の実装
 *
 * systemdのエラー本文は利用者へ返さず、成否だけを呼び出し元へ伝える
 * エラー本文にはunit名やパスが含まれ、また応答の形式はsystemdの版によって変わり得るため
 */

#include "systemdtimeroperation.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QRegularExpression>
#include <QDebug>

namespace qsnapper::dbusservice {

    namespace {
        const QString SystemdService = QStringLiteral("org.freedesktop.systemd1");                  // systemdのバス名
        const QString SystemdPath = QStringLiteral("/org/freedesktop/systemd1");                    // Managerのオブジェクトパス
        const QString ManagerInterface = QStringLiteral("org.freedesktop.systemd1.Manager");        // Managerのインタフェース
        const QString UnitInterface = QStringLiteral("org.freedesktop.systemd1.Unit");              // unitのインタフェース
        const QString PropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");      // プロパティの取得に使うインタフェース
        const QString NoSuchUnitError = QStringLiteral("org.freedesktop.systemd1.NoSuchUnit");      // unitが読み込まれていない
        const QString FileNotFoundError = QStringLiteral("org.freedesktop.DBus.Error.FileNotFound"); // unitファイルが存在しない
        const QString NotFoundState = QStringLiteral("not-found");                                  // unitファイルが存在しない場合のUnitFileState
        const QString UnknownState = QStringLiteral("unknown");                                     // unitファイルが存在しない場合のActiveState
        const QString InactiveState = QStringLiteral("inactive");                                   // unitが読み込まれていない場合のActiveState

        /**
         * @brief 応答がsystemdからの正常な応答かどうかを判定する
         * @param reply 応答
         * @return 正常な応答の場合はtrue
         */
        bool isMethodReturn(const QDBusMessage &reply)
        {
            return reply.type() == QDBusMessage::ReplyMessage;
        }
    } // namespace

    SystemdTimerOperation::SystemdTimerOperation(const QDBusConnection &bus, Kind kind, const QStringList &units,
                                                 MutationGuard mutationGuard, QObject *parent)
        : QObject(parent),
          m_bus(bus),
          m_kind(kind),
          m_units(units),
          m_mutationGuard(std::move(mutationGuard))
    {
        m_deadline.setSingleShot(true);
        m_deadline.setInterval(kind == Kind::Query ? QueryDeadlineMs : ChangeDeadlineMs);
        connect(&m_deadline, &QTimer::timeout, this, [this]() {
            qWarning() << "Timer operation exceeded its deadline";
            finish(Outcome::Failed);
        });

        m_jobTimeout.setSingleShot(true);
        m_jobTimeout.setInterval(JobTimeoutMs);
        connect(&m_jobTimeout, &QTimer::timeout, this, [this]() {
            if (!m_jobFinished) {
                return;
            }
            qWarning() << "Timer unit job did not finish in time";
            m_waitingJob.clear();
            auto next = std::move(m_jobFinished);
            m_jobFinished = nullptr;
            next(false);
        });
    }

    bool SystemdTimerOperation::isStateToken(const QString &value)
    {
        static const QRegularExpression pattern(QRegularExpression::anchoredPattern(QStringLiteral("[a-z][a-z-]{0,63}")));
        return pattern.match(value).hasMatch();
    }

    /**
     * @brief 操作を開始する
     *
     * 最初の呼び出しはQTimer::singleShotで次のイベントループへ送る
     * 呼び出し元 (D-Busスロット) が戻る前に完了の通知が走ると、応答の送信順序が呼び出し元の想定と異なるため
     */
    void SystemdTimerOperation::start(Completion completion)
    {
        m_completion = std::move(completion);

        // JobRemovedは、systemdがjobを要求した接続へ必ず送るため、Subscribeを呼ばずに受け取れる
        // 送信元をsystemd1のバス名に限定し、他のクライアントが偽のシグナルを送ってjobの完了を装えないようにする
        if (m_kind != Kind::Query) {
            m_jobSignalConnected = m_bus.connect(SystemdService, SystemdPath, ManagerInterface, QStringLiteral("JobRemoved"),
                                                 this, SLOT(handleJobRemoved(uint, QDBusObjectPath, QString, QString)));
            if (!m_jobSignalConnected) {
                qWarning() << "Failed to subscribe to systemd job notifications";
            }
        }

        m_deadline.start();

        QTimer::singleShot(0, this, [this]() {
            if (m_finished) {
                return;
            }
            if (m_kind == Kind::Query) {
                queryUnit(0, [this]() { finish(Outcome::Succeeded); });
                return;
            }

            if (!m_jobSignalConnected) {
                finish(Outcome::Failed);
                return;
            }

            const QString unit = m_units.value(0);
            if (m_kind == Kind::Enable) {
                if (!mayMutate()) {
                    return;
                }
                callManager(QStringLiteral("EnableUnitFiles"),
                            {QStringList{unit}, false, false}, CallTimeoutMs,
                            [this](const QDBusMessage &reply) {
                    if (!isMethodReturn(reply)) {
                        qWarning() << "systemd rejected the request to enable a timer";
                        verifyAndFinish(Outcome::Failed);
                        return;
                    }
                    if (!mayMutate()) {
                        return;
                    }
                    callManager(QStringLiteral("Reload"), {}, ReloadTimeoutMs, [this](const QDBusMessage &reloadReply) {
                        if (!isMethodReturn(reloadReply)) {
                            qWarning() << "systemd failed to reload unit files";
                            verifyAndFinish(Outcome::Failed);
                            return;
                        }
                        if (!mayMutate()) {
                            return;
                        }
                        runUnitJob(QStringLiteral("StartUnit"), [this](bool done) {
                            if (!done) {
                                verifyAndFinish(Outcome::Failed);
                                return;
                            }
                            verifyAndFinish(Outcome::Succeeded);
                        });
                    });
                });
                return;
            }

            // 無効化では、先に停止してから起動時の設定を外す
            // 先に無効化すると、停止に失敗したときに、動いているのに無効と表示される状態が残るため
            if (!mayMutate()) {
                return;
            }
            runUnitJob(QStringLiteral("StopUnit"), [this, unit](bool done) {
                if (!done) {
                    verifyAndFinish(Outcome::Failed);
                    return;
                }
                if (!mayMutate()) {
                    return;
                }
                callManager(QStringLiteral("DisableUnitFiles"), {QStringList{unit}, false}, CallTimeoutMs,
                            [this](const QDBusMessage &reply) {
                    if (!isMethodReturn(reply)) {
                        qWarning() << "systemd rejected the request to disable a timer";
                        verifyAndFinish(Outcome::Failed);
                        return;
                    }
                    if (!mayMutate()) {
                        return;
                    }
                    callManager(QStringLiteral("Reload"), {}, ReloadTimeoutMs, [this](const QDBusMessage &reloadReply) {
                        if (!isMethodReturn(reloadReply)) {
                            qWarning() << "systemd failed to reload unit files";
                            verifyAndFinish(Outcome::Failed);
                            return;
                        }
                        verifyAndFinish(Outcome::Succeeded);
                    });
                });
            });
        });
    }

    void SystemdTimerOperation::callManager(const QString &method, const QVariantList &arguments, int timeoutMs,
                                            ReplyHandler handler)
    {
        QDBusMessage message = QDBusMessage::createMethodCall(SystemdService, SystemdPath, ManagerInterface, method);
        message.setArguments(arguments);
        callSystemd(message, timeoutMs, std::move(handler));
    }

    /**
     * @brief systemd1へ非同期に呼び出しを送る
     *
     * 応答はQDBusPendingCallWatcherで受け取り、本オブジェクトが終わった後に届いた応答は捨てる
     * 期限はQt D-Busの呼び出しの期限で設け、systemdが応答しない場合もエラー応答として扱う
     */
    void SystemdTimerOperation::callSystemd(const QDBusMessage &message, int timeoutMs, ReplyHandler handler)
    {
        QDBusPendingCall pending = m_bus.asyncCall(message, timeoutMs);
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, handler = std::move(handler)](QDBusPendingCallWatcher *self) {
            self->deleteLater();
            if (m_finished) {
                return;
            }
            handler(self->reply());
        });
    }

    /**
     * @brief unitの状態を順に読む
     *
     * unitファイルが存在しないことが確定した場合 (FileNotFoundまたはNoSuchUnit) だけ、not-foundとして扱う
     * 期限切れや権限の不足は、存在しないことの証拠にならないため、操作全体の失敗にする
     */
    void SystemdTimerOperation::queryUnit(int index, std::function<void()> next)
    {
        if (index >= m_units.size()) {
            next();
            return;
        }

        const QString unit = m_units.at(index);
        callManager(QStringLiteral("GetUnitFileState"), {unit}, CallTimeoutMs,
                    [this, index, unit, next = std::move(next)](const QDBusMessage &reply) mutable {
            if (!isMethodReturn(reply)) {
                if (reply.errorName() == FileNotFoundError || reply.errorName() == NoSuchUnitError) {
                    m_states.insert(unit, SnapperTimerState{NotFoundState, UnknownState});
                    queryUnit(index + 1, std::move(next));
                    return;
                }
                qWarning() << "Failed to read a timer unit file state";
                finish(Outcome::Failed);
                return;
            }

            const QString fileState = reply.arguments().value(0).toString();
            if (reply.arguments().size() != 1 || !isStateToken(fileState)) {
                qWarning() << "systemd returned an unexpected timer unit file state";
                finish(Outcome::Failed);
                return;
            }

            callManager(QStringLiteral("GetUnit"), {unit}, CallTimeoutMs,
                        [this, index, unit, fileState, next = std::move(next)](const QDBusMessage &unitReply) mutable {
                // 読み込まれていないunitは、起動していないことが確定しているためinactiveとする
                if (!isMethodReturn(unitReply)) {
                    if (unitReply.errorName() == NoSuchUnitError) {
                        m_states.insert(unit, SnapperTimerState{fileState, InactiveState});
                        queryUnit(index + 1, std::move(next));
                        return;
                    }
                    qWarning() << "Failed to look up a timer unit";
                    finish(Outcome::Failed);
                    return;
                }

                const QVariant pathValue = unitReply.arguments().value(0);
                if (unitReply.arguments().size() != 1 || pathValue.userType() != qMetaTypeId<QDBusObjectPath>()) {
                    qWarning() << "systemd returned an unexpected unit object path";
                    finish(Outcome::Failed);
                    return;
                }

                // systemdが返したオブジェクトパスを使い、unit名からパスを組み立てない
                // unit名のエスケープ規則を本サービスで再現すると、規則の差異で別のunitを読む恐れがあるため
                QDBusMessage get = QDBusMessage::createMethodCall(SystemdService, pathValue.value<QDBusObjectPath>().path(),
                                                                  PropertiesInterface, QStringLiteral("Get"));
                get.setArguments({UnitInterface, QStringLiteral("ActiveState")});
                callSystemd(get, CallTimeoutMs,
                            [this, index, unit, fileState, next = std::move(next)](const QDBusMessage &getReply) mutable {
                    if (!isMethodReturn(getReply) || getReply.arguments().size() != 1) {
                        qWarning() << "Failed to read a timer active state";
                        finish(Outcome::Failed);
                        return;
                    }
                    const QString activeState = getReply.arguments().at(0).value<QDBusVariant>().variant().toString();
                    if (!isStateToken(activeState)) {
                        qWarning() << "systemd returned an unexpected timer active state";
                        finish(Outcome::Failed);
                        return;
                    }
                    m_states.insert(unit, SnapperTimerState{fileState, activeState});
                    queryUnit(index + 1, std::move(next));
                });
            });
        });
    }

    /**
     * @brief 変更の直前に、変更を続けてよいかを判定する
     *
     * 止めた場合は、それまでに行った変更を巻き戻さない
     * 巻き戻しも変更であり、止めた理由 (復元の開始、呼び出し元の切断) の下では行うべきではないため
     * 代わりに状態を読み直し、実際の状態を呼び出し元へ渡す
     */
    bool SystemdTimerOperation::mayMutate()
    {
        if (m_finished) {
            return false;
        }
        if (m_mutationGuard && m_mutationGuard()) {
            return true;
        }
        m_interrupted = true;
        m_states.clear();
        queryUnit(0, [this]() { finish(Outcome::Interrupted); });
        return false;
    }

    /**
     * @brief StartUnitまたはStopUnitを呼び、jobの完了を待つ
     *
     * StartUnitとStopUnitはjobを登録した時点で応答するため、成否はJobRemovedシグナルの結果で判断する
     * 応答よりも先にシグナルが届く場合があるため、届いたjobの結果を一時的に保持し、応答を受けた時点で照合する
     */
    void SystemdTimerOperation::runUnitJob(const QString &method, std::function<void(bool)> next)
    {
        callManager(method, {m_units.value(0), QStringLiteral("replace")}, CallTimeoutMs,
                    [this, next = std::move(next)](const QDBusMessage &reply) mutable {
            const QVariant jobValue = reply.arguments().value(0);
            if (!isMethodReturn(reply) || reply.arguments().size() != 1
                || jobValue.userType() != qMetaTypeId<QDBusObjectPath>()) {
                qWarning() << "systemd rejected a timer unit job";
                next(false);
                return;
            }

            const QString job = jobValue.value<QDBusObjectPath>().path();
            const auto early = m_earlyJobResults.constFind(job);
            if (early != m_earlyJobResults.constEnd()) {
                const bool done = early.value() == QStringLiteral("done");
                m_earlyJobResults.clear();
                next(done);
                return;
            }
            m_earlyJobResults.clear();
            m_waitingJob = job;
            m_jobFinished = std::move(next);
            m_jobTimeout.start();
        });
    }

    void SystemdTimerOperation::handleJobRemoved(uint id, const QDBusObjectPath &job, const QString &unit,
                                                 const QString &result)
    {
        Q_UNUSED(id);
        if (m_finished || unit != m_units.value(0)) {
            return;
        }

        if (m_waitingJob.isEmpty()) {
            // 応答より先に届いた結果は上限を設けて保持し、他のunitのjobが大量に完了しても際限なく増えないようにする
            if (m_earlyJobResults.size() < MaxBufferedJobResults) {
                m_earlyJobResults.insert(job.path(), result);
            }
            return;
        }
        if (job.path() != m_waitingJob || !m_jobFinished) {
            return;
        }

        m_jobTimeout.stop();
        m_waitingJob.clear();
        auto next = std::move(m_jobFinished);
        m_jobFinished = nullptr;
        if (result != QStringLiteral("done")) {
            qWarning() << "A timer unit job did not complete successfully";
        }
        next(result == QStringLiteral("done"));
    }

    /**
     * @brief 状態を読み直し、要求どおりかどうかで操作を終える
     *
     * systemdの各呼び出しが成功しても、unitの状態が要求どおりとは限らない (maskされている、依存関係で起動できないなど)
     * そのため、成否は読み直した状態だけで決める
     */
    void SystemdTimerOperation::verifyAndFinish(Outcome outcomeIfMatched)
    {
        m_states.clear();
        queryUnit(0, [this, outcomeIfMatched]() {
            if (outcomeIfMatched == Outcome::Succeeded && stateMatchesRequest()) {
                finish(Outcome::Succeeded);
                return;
            }
            finish(Outcome::Failed);
        });
    }

    bool SystemdTimerOperation::stateMatchesRequest() const
    {
        const auto it = m_states.constFind(m_units.value(0));
        if (it == m_states.constEnd()) {
            return false;
        }
        if (m_kind == Kind::Enable) {
            return it->unitFileState == QStringLiteral("enabled") && it->activeState == QStringLiteral("active");
        }
        if (m_kind == Kind::Disable) {
            // maskされたunitは起動しないため、無効化の目的は満たしている
            // ただし本サービスはunmaskもmaskもしない
            const bool notEnabled = it->unitFileState == QStringLiteral("disabled")
                                    || it->unitFileState == QStringLiteral("masked")
                                    || it->unitFileState == QStringLiteral("masked-runtime");
            return notEnabled && it->activeState == QStringLiteral("inactive");
        }
        return true;
    }

    /**
     * @brief 操作を終える
     *
     * 期限切れと応答が同じ周回で重なっても、完了の通知は1回だけにする
     * D-Busの応答を2回送ると、呼び出し元が別の応答と取り違える恐れがあるため
     */
    void SystemdTimerOperation::finish(Outcome outcome)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        // 変更を止めた後の読み直しが失敗または期限切れになっても、呼び出し元が止めた理由 (復元の実行中など) を返せるようにする
        if (m_interrupted) {
            outcome = Outcome::Interrupted;
        }
        m_deadline.stop();
        m_jobTimeout.stop();
        m_jobFinished = nullptr;
        if (m_jobSignalConnected) {
            m_bus.disconnect(SystemdService, SystemdPath, ManagerInterface, QStringLiteral("JobRemoved"),
                             this, SLOT(handleJobRemoved(uint, QDBusObjectPath, QString, QString)));
            m_jobSignalConnected = false;
        }

        auto completion = std::move(m_completion);
        m_completion = nullptr;
        if (completion) {
            completion(outcome, outcome == Outcome::Failed && m_kind == Kind::Query ? SnapperTimerStateMap{} : m_states);
        }
    }

} // namespace qsnapper::dbusservice
