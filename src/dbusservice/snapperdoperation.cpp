/**
 * @file snapperdoperation.cpp
 * @brief snapperdへの変更の転送の実装
 *
 * snapperdのエラー本文には、config名やパスが含まれ得るため、本ファイルはエラー本文を読まず、エラー名だけで分類する
 */

#include "snapperdoperation.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QMap>
#include <QSet>
#include <QVariant>

namespace qsnapper::dbusservice {

    const QString SnapperdOperation::Service = QStringLiteral("org.opensuse.Snapper");
    const QString SnapperdOperation::Path = QStringLiteral("/org/opensuse/Snapper");
    const QString SnapperdOperation::Interface = QStringLiteral("org.opensuse.Snapper");

    SnapperdOperation::SnapperdOperation(const QDBusConnection &bus, const QString &caller,
                                         const QDBusMessage &mutation, const QString &listConfig,
                                         const QList<uint> &requiredNumbers, MutationGuard mutationGuard,
                                         QObject *parent)
        : QObject(parent),
          m_bus(bus),
          m_caller(caller),
          m_mutation(mutation),
          m_listConfig(listConfig),
          m_requiredNumbers(requiredNumbers),
          m_mutationGuard(std::move(mutationGuard))
    {
        m_replyDeadline.setSingleShot(true);
        connect(&m_replyDeadline, &QTimer::timeout, this, [this]() {
            if (!m_finished && m_onDeadline) {
                m_onDeadline();
            }
        });
    }

    void SnapperdOperation::start(int replyDeadlineMs, DeadlineHandler onDeadline, Completion completion)
    {
        m_onDeadline = std::move(onDeadline);
        m_completion = std::move(completion);
        m_replyDeadline.start(replyDeadlineMs);

        // 呼び出し元の切断は、監視を始めてから現在の接続の有無を確かめることで、見落としなく検出する
        // 先に確かめてから監視を始めると、その間に切断された場合を見落とすため
        m_callerWatcher = new QDBusServiceWatcher(m_caller, m_bus, QDBusServiceWatcher::WatchForUnregistration, this);
        connect(m_callerWatcher, &QDBusServiceWatcher::serviceUnregistered, this,
                [this](const QString &) { m_callerGone = true; });

        // 呼び出し元の接続の有無は、同期呼び出しで待つと他の要求が止まるため、非同期に確かめる
        QDBusMessage query = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
        query << m_caller;
        call(query, QueryTimeoutMs, [this](const QDBusMessage &reply) {
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1
                    || !reply.arguments().constFirst().toBool()) {
                m_callerGone = true;
            }
            if (m_callerGone) {
                finish(Outcome::Interrupted);
                return;
            }
            resolveSnapperdOwner(false);
        });
    }

    void SnapperdOperation::resolveSnapperdOwner(bool activated)
    {
        QDBusMessage getOwner = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        getOwner << Service;
        call(getOwner, QueryTimeoutMs, [this, activated](const QDBusMessage &owner) {
            const QString uniqueName = owner.type() == QDBusMessage::ReplyMessage && owner.arguments().size() == 1
                                           ? owner.arguments().constFirst().toString()
                                           : QString();
            if (!uniqueName.startsWith(QLatin1Char(':'))) {
                // snapperdはアイドル時に終了するため、所有者がいなければ起動させてから取得し直す
                // 起動の後も取得できない場合は、変更を転送しない
                if (activated || owner.errorName() != QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner")) {
                    finish(Outcome::NotForwarded);
                    return;
                }
                QDBusMessage startService = QDBusMessage::createMethodCall(
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
                startService << Service << 0U;
                call(startService, QueryTimeoutMs, [this](const QDBusMessage &started) {
                    if (started.type() != QDBusMessage::ReplyMessage) {
                        finish(Outcome::NotForwarded);
                        return;
                    }
                    resolveSnapperdOwner(true);
                });
                return;
            }
            m_snapperdOwner = uniqueName;

            // 取得した接続の切断だけを監視し、別のsnapperdへ所有者が移った場合と区別する
            // 監視を始める前に切断していた場合は、以降の照会と変更が宛先なしで失敗するため、変更は行われない
            m_snapperdWatcher = new QDBusServiceWatcher(m_snapperdOwner, m_bus,
                                                        QDBusServiceWatcher::WatchForUnregistration, this);
            connect(m_snapperdWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &) {
                m_snapperdLost = true;
                if (m_awaitingSettlement) {
                    finish(Outcome::Unknown);
                }
            });

            if (m_listConfig.isEmpty()) {
                forwardMutation();
            }
            else {
                checkSnapshotList();
            }
        });
    }

    QDBusMessage SnapperdOperation::addressedToSnapperd(const QDBusMessage &message) const
    {
        QDBusMessage addressed = QDBusMessage::createMethodCall(m_snapperdOwner, message.path(),
                                                                message.interface(), message.member());
        addressed.setArguments(message.arguments());
        return addressed;
    }

    bool SnapperdOperation::isNotForwardedError(const QString &errorName)
    {
        static const QSet<QString> notForwarded = {
            QDBusError::errorString(QDBusError::ServiceUnknown),
            QDBusError::errorString(QDBusError::AccessDenied),
            QDBusError::errorString(QDBusError::UnknownMethod),
            QDBusError::errorString(QDBusError::UnknownObject),
            QDBusError::errorString(QDBusError::UnknownInterface),
            QDBusError::errorString(QDBusError::InvalidSignature),
            QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner"),
        };
        // 自動起動の失敗 (org.freedesktop.DBus.Error.Spawn.*) も、snapperdが動いていないため変更されていない
        return notForwarded.contains(errorName)
               || errorName.startsWith(QStringLiteral("org.freedesktop.DBus.Error.Spawn."));
    }

    void SnapperdOperation::call(const QDBusMessage &message, int timeoutMs, ReplyHandler handler)
    {
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, timeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, handler = std::move(handler)](QDBusPendingCallWatcher *finishedWatcher) {
                    finishedWatcher->deleteLater();
                    if (m_finished) {
                        return;
                    }
                    handler(finishedWatcher->reply());
                });
    }

    void SnapperdOperation::checkSnapshotList()
    {
        QDBusMessage list = QDBusMessage::createMethodCall(m_snapperdOwner, Path, Interface,
                                                           QStringLiteral("ListSnapshots"));
        list << m_listConfig;
        call(list, QueryTimeoutMs, [this](const QDBusMessage &reply) {
            if (reply.type() != QDBusMessage::ReplyMessage) {
                finish(Outcome::NotForwarded);
                return;
            }
            QList<uint> listed;
            if (!parseSnapshotNumbers(reply, &listed)) {
                finish(Outcome::NotForwarded);
                return;
            }
            const QSet<uint> present(listed.cbegin(), listed.cend());
            for (uint number : std::as_const(m_requiredNumbers)) {
                if (!present.contains(number)) {
                    finish(Outcome::Outdated);
                    return;
                }
            }
            forwardMutation();
        });
    }

    bool SnapperdOperation::parseSnapshotNumbers(const QDBusMessage &reply, QList<uint> *numbers)
    {
        // snapperdのListSnapshotsは a(uquxussa{ss}) を返す (番号、種類、Preの番号、日時、UID、説明、cleanup、userdata)
        if (reply.arguments().size() != 1 || reply.signature() != QStringLiteral("a(uquxussa{ss})")) {
            return false;
        }
        const QVariant value = reply.arguments().constFirst();
        if (!value.canConvert<QDBusArgument>()) {
            return false;
        }
        const QDBusArgument argument = value.value<QDBusArgument>();
        argument.beginArray();
        while (!argument.atEnd()) {
            if (numbers->size() >= MaxListedSnapshots) {
                return false;
            }
            uint number = 0;
            ushort type = 0;
            uint preNumber = 0;
            qlonglong date = 0;
            uint uid = 0;
            QString description;
            QString cleanup;
            QMap<QString, QString> userdata;
            argument.beginStructure();
            argument >> number >> type >> preNumber >> date >> uid >> description >> cleanup >> userdata;
            argument.endStructure();
            numbers->append(number);
        }
        argument.endArray();
        return true;
    }

    void SnapperdOperation::forwardMutation()
    {
        if (m_callerGone || !m_mutationGuard || !m_mutationGuard()) {
            finish(Outcome::Interrupted);
            return;
        }
        // 一覧を確かめた後にsnapperdが入れ替わった場合、新しいsnapperdへ変更を送らない
        if (m_snapperdLost) {
            finish(Outcome::NotForwarded);
            return;
        }

        // 宛先をユニーク名に固定するため、別のsnapperdへ所有者が移っていれば宛先なしで失敗し、変更は行われない
        call(addressedToSnapperd(m_mutation), m_mutationTimeoutMs, [this](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ReplyMessage) {
                finish(Outcome::Replied);
                return;
            }
            // snapperd自身が返したエラーは、変更を受け取って処理した結果として扱う
            // 送信元が取得したユニーク名と一致しない場合だけ、バスが配送しなかった可能性を判定する
            if (reply.service() == m_snapperdOwner) {
                finish(Outcome::Rejected);
                return;
            }
            const QString errorName = reply.errorName();
            if (isNotForwardedError(errorName)) {
                finish(Outcome::NotForwarded);
                return;
            }
            if (errorName == QDBusError::errorString(QDBusError::Disconnected)) {
                // 本サービス自身のバス接続が切れた場合は、照会も送れないため、結果が分からないまま完了しない
                // 完了しない間は他の変更を拒否し続け、結果が分からない状態で次の変更を受け付けない
                m_awaitingSettlement = true;
                return;
            }
            if (errorName == QDBusError::errorString(QDBusError::NoReply)
                    || errorName == QDBusError::errorString(QDBusError::Timeout)) {
                m_awaitingSettlement = true;
                sendBarrier();
                return;
            }
            finish(Outcome::Rejected);
        });
    }

    void SnapperdOperation::sendBarrier()
    {
        if (m_snapperdLost) {
            finish(Outcome::Unknown);
            return;
        }
        QDBusMessage barrier = QDBusMessage::createMethodCall(m_snapperdOwner, Path, Interface, m_barrierMember);
        call(barrier, BarrierTimeoutMs, [this](const QDBusMessage &reply) {
            // snapperdは同じ接続からの呼び出しを到着順に処理するため、照会への応答はエラーでも変更の処理の終わりを示す
            if (reply.type() == QDBusMessage::ReplyMessage || reply.service() == m_snapperdOwner) {
                finish(Outcome::Settled);
                return;
            }
            const QString errorName = reply.errorName();
            // ユニーク名は再利用されないため、宛先がないという応答は変更を受け取った接続が終了したことを示す
            if (errorName == QDBusError::errorString(QDBusError::ServiceUnknown)
                    || errorName == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner")) {
                finish(Outcome::Unknown);
                return;
            }
            // 本サービス自身のバス接続が切れた場合は、照会も送れないため、結果が分からないまま完了しない
            if (errorName == QDBusError::errorString(QDBusError::Disconnected)) {
                return;
            }
            // 照会の時間切れ、またはバスが照会を拒否した場合 (ポリシーや上限) は、snapperdの処理の終わりを確かめられていない
            // 排他を保ったまま間隔を空けて送り直し、snapperdの応答か接続の終了を確かめるまで完了しない
            if (errorName == QDBusError::errorString(QDBusError::NoReply)
                    || errorName == QDBusError::errorString(QDBusError::Timeout)) {
                sendBarrier();
                return;
            }
            QTimer::singleShot(BarrierRetryMs, this, [this]() {
                if (!m_finished) {
                    sendBarrier();
                }
            });
        });
    }

    void SnapperdOperation::finish(Outcome outcome)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_replyDeadline.stop();
        if (m_completion) {
            m_completion(outcome);
        }
    }

} // namespace qsnapper::dbusservice
