/**
 * @file snapperdop_test_driver.cpp
 * @brief SnapperdOperationの1つのシナリオを隔離バス上で実行し、結果を標準出力へ報告する
 *
 * tests/integration/test_snapperd_operation.pyが、偽のsnapperd (fake_snapperd.py) の状態を切り替えて本ドライバを起動する
 * ドライバは実snapperdを起動せず、偽物または不在の宛先だけを相手にする
 * シナリオは第1引数の名前で選び、次の結果を1行ずつ出力する
 *   COMPLETION <Outcome名> : SnapperdOperationが完了を通知した (1回だけ)
 *   DEADLINE               : 応答の期限が完了より先に来た (1回だけ)
 *   SUMMARY deadline=<数> completion=<数> : 通知の回数
 *
 * 呼び出し元の切断によるInterruptedは、切断を検出した後に結果を返す接続が残らないため、
 * このドライバでは再現しない (切断の検出そのものは本番の呼び出し元の接続でしか起こせない)
 */

#include "snapperdoperation.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <cstdio>

using qsnapper::dbusservice::SnapperdOperation;

namespace {

    /**
     * @brief Outcomeを表示用の名前に変換する
     * @param outcome 変換する結果
     * @return 表示用の名前
     */
    QString outcomeName(SnapperdOperation::Outcome outcome)
    {
        switch (outcome) {
        case SnapperdOperation::Outcome::Replied:
            return QStringLiteral("Replied");
        case SnapperdOperation::Outcome::Rejected:
            return QStringLiteral("Rejected");
        case SnapperdOperation::Outcome::NotForwarded:
            return QStringLiteral("NotForwarded");
        case SnapperdOperation::Outcome::Outdated:
            return QStringLiteral("Outdated");
        case SnapperdOperation::Outcome::Interrupted:
            return QStringLiteral("Interrupted");
        case SnapperdOperation::Outcome::Settled:
            return QStringLiteral("Settled");
        case SnapperdOperation::Outcome::Unknown:
            return QStringLiteral("Unknown");
        }
        return QStringLiteral("Invalid");
    }

} // namespace

/**
 * @brief 指定されたシナリオを実行する
 * @param argc 引数の数
 * @param argv 引数 (argv[1]にシナリオ名)
 * @return 0 = 完了を1回通知した、3 = 期限内に完了しなかった、64 = 引数不正
 */
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 64;
    }

    const QString scenario = QString::fromLocal8Bit(argv[1]);

    // 期限切れの検査だけ短い期限にし、それ以外は偽のsnapperdの応答を待てる長さにする
    const int deadlineMs = scenario == QStringLiteral("deadline") ? 1000 : 15000;

    // ガードが偽を返す検査では、一覧の照会の後に変更を止める
    const auto guard = [&scenario]() {
        return scenario != QStringLiteral("interrupted");
    };

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        std::fprintf(stderr, "ERROR: cannot connect to the system bus\n");
        return 2;
    }

    // 実snapperdと同じ宛先とメソッド名を使い、転送そのものを偽物に受け取らせる
    QDBusMessage mutation = QDBusMessage::createMethodCall(
        SnapperdOperation::Service, SnapperdOperation::Path, SnapperdOperation::Interface,
        QStringLiteral("DeleteSnapshots"));
    mutation << QStringLiteral("qsnapper-snapperd-test")
             << QVariant::fromValue(QList<uint>{4242});

    int deadlines = 0;
    int completions = 0;

    SnapperdOperation operation(bus, bus.baseService(), mutation,
                                QStringLiteral("qsnapper-snapperd-test"), QList<uint>{4242},
                                guard, &app);

    // 応答を失った場合の検査では、変更の転送の期限を短くして、時間切れの後の完了の確認を再現する
    if (scenario == QStringLiteral("settled") || scenario == QStringLiteral("ownerswap")
            || scenario == QStringLiteral("barrierdenied")) {
        operation.setMutationTimeoutMs(1000);
    }
    // 隔離バスのポリシーで拒否されるメソッド名を照会に使い、バスが照会を拒否した場合を再現する
    if (scenario == QStringLiteral("barrierdenied")) {
        operation.setBarrierMemberForTesting(QStringLiteral("DeniedBarrier"));
    }

    auto *watchdog = new QTimer(&app);
    watchdog->setSingleShot(true);
    QObject::connect(watchdog, &QTimer::timeout, &app, [&app]() {
        std::printf("TIMEOUT\n");
        std::fflush(stdout);
        app.exit(3);
    });
    watchdog->start(30000);

    operation.start(deadlineMs,
                    [&deadlines]() {
                        ++deadlines;
                        std::printf("DEADLINE\n");
                        std::fflush(stdout);
                    },
                    [&app, &completions, &deadlines](SnapperdOperation::Outcome outcome) {
                        ++completions;
                        std::printf("COMPLETION %s\n", qPrintable(outcomeName(outcome)));
                        std::printf("SUMMARY deadline=%d completion=%d\n", deadlines, completions);
                        std::fflush(stdout);
                        app.quit();
                    });

    const int result = app.exec();
    if (result != 0) {
        return result;
    }
    return completions == 1 ? 0 : 3;
}
