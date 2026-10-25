/**
 * @file restoregate_test_service.cpp
 * @brief 隔離root D-Busバス上で、復元の実行中に非復元操作を拒否する共通ゲートを検証するためのテスト専用サービス
 *
 * 本番のSnapshotOperations (src/dbusservice/snapshotoperations*.cpp) をそのままリンクし、D-Busへ登録する
 * QSNAPPER_ENABLE_TEST_HOOKSを定義してコンパイルし、次の2点だけをテスト用フックで差し替える
 *   - polkitへの問い合わせ: 隔離環境にはpolkitdが無いため、即時許可または保留 (遅延許可) にする
 *   - 復元の実行中の状態: 実際のmountを伴わずに作る
 * 認可から本体の実行までの経路 (authorizeThen / completeDeferredCall) は本番のコードのまま通る
 *
 * D-Busの送信許可は本番のACLだけを使うため、テストからの制御は標準入力の行コマンドで行う
 * 各コマンドの結果は標準出力へ1行で返す
 *   auth grant | auth defer     : 以降の認可要求を即時許可にする | 保留する
 *   pending                     : 保留中の認可の件数を "PENDING <件数>" で返す
 *   resolve grant | resolve deny: 保留中の認可をすべて許可 | 拒否で確定する
 *   restore begin <config名>    : 復元の実行中の状態を作る
 *   restore end                 : 復元の実行中の状態を解除する
 *   snapperd begin | snapperd end : snapperdへ転送した変更の実行中の状態を作る | 解除する
 *
 * インストールせず、add_test()にも登録しない
 * (tests/integration/run_isolated_restore_plan_test.sh経由でのみ起動する)
 */

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QFile>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QSocketNotifier>
#include <QString>

#include <cstdio>
#include <unistd.h>

#include "snapshotoperations.h"

namespace {

/**
 * @brief 標準出力へ1行を書き、テスト側が直ちに読めるようにflushする
 * @param line 改行を含まない応答
 */
void writeResponse(const QByteArray &line)
{
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/**
 * @brief 標準入力の行コマンドを1件実行する
 * @param operations 操作対象のサービス
 * @param command 改行を除いたコマンド
 */
void runCommand(SnapshotOperations &operations, const QByteArray &command)
{
    const QList<QByteArray> words = command.trimmed().split(' ');
    const QByteArray verb = words.value(0);
    const QByteArray argument = words.value(1);

    if (verb == "auth" && argument == "grant") {
        operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    }
    else if (verb == "auth" && argument == "defer") {
        operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Defer);
    }
    else if (verb == "pending") {
        writeResponse("PENDING " + QByteArray::number(operations.pendingTestAuthorizationCount()));
        return;
    }
    else if (verb == "resolve" && (argument == "grant" || argument == "deny")) {
        operations.resolveTestAuthorizations(argument == "grant");
    }
    else if (verb == "restore" && argument == "begin" && words.size() == 3) {
        operations.beginRestoreExecutionForTesting(QString::fromUtf8(words.at(2)));
    }
    else if (verb == "restore" && argument == "end") {
        operations.endRestoreExecutionForTesting();
    }
    else if (verb == "snapperd" && argument == "begin") {
        operations.beginSnapperdOperationForTesting();
    }
    else if (verb == "snapperd" && argument == "end") {
        operations.endSnapperdOperationForTesting();
    }
    else {
        writeResponse("ERROR unknown command");
        return;
    }
    writeResponse("OK");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 本番のmain.cppと同じく、a{ss}を受け取るメソッド (WriteSnapperConfigなど) を公開するために登録する
    qDBusRegisterMetaType<QMap<QString, QString>>();
    // GetTimerStatesを公開するため、本番と同じくa{s(ss)}の型を登録する
    qDBusRegisterMetaType<SnapperTimerState>();
    qDBusRegisterMetaType<SnapperTimerStateMap>();

    QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        std::fprintf(stderr, "ERROR: Cannot connect to the D-Bus system bus\n");
        return 1;
    }

    // 本番のmain.cppと同じく、オブジェクトを先に登録してからサービス名を取得する
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    if (!connection.registerObject(QStringLiteral("/com/presire/qsnapper/Operations"), &operations,
                                   QDBusConnection::ExportAllSlots
                                       | QDBusConnection::ExportAllSignals)) {
        std::fprintf(stderr, "ERROR: Failed to register D-Bus object: %s\n",
                     qPrintable(connection.lastError().message()));
        return 1;
    }
    if (!connection.registerService(QStringLiteral("com.presire.qsnapper.Operations"))) {
        std::fprintf(stderr, "ERROR: Failed to register D-Bus service: %s\n",
                     qPrintable(connection.lastError().message()));
        return 1;
    }

    // 1回の通知で複数行が届いても取りこぼさないよう、読んだ分を貯めてから行単位で処理する
    // 標準入力が閉じたら (テスト側の終了) サービスも終了する
    QByteArray pendingInput;
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, &app, [&]() {
        char buffer[512];
        const ssize_t length = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length <= 0) {
            stdinNotifier.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        pendingInput.append(buffer, static_cast<qsizetype>(length));
        qsizetype newline = -1;
        while ((newline = pendingInput.indexOf('\n')) >= 0) {
            const QByteArray line = pendingInput.left(newline);
            pendingInput.remove(0, newline + 1);
            runCommand(operations, line);
        }
    });

    const QString readyFilePath = QString::fromLocal8Bit(qgetenv("QSNAPPER_TEST_READY_FILE"));
    if (!readyFilePath.isEmpty()) {
        QFile readyFile(readyFilePath);
        if (readyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            readyFile.write("READY");
            readyFile.close();
        }
    }

    return app.exec();
}
