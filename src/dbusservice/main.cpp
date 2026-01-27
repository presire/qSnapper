#include "snapshotoperations.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName("Presire");
    app.setApplicationName("qSnapper D-Bus Service");
    app.setApplicationVersion("1.0.0");

    // D-Busシステムバスに接続
    QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        qCritical() << "Cannot connect to the D-Bus system bus";
        return 1;
    }

    // サービスを登録
    if (!connection.registerService("com.presire.qsnapper.Operations")) {
        qCritical() << "Failed to register D-Bus service:" << connection.lastError().message();
        return 1;
    }

    // オブジェクトを作成して登録 (シグナルもエクスポート)
    SnapshotOperations operations;
    if (!connection.registerObject("/com/presire/qsnapper/Operations", &operations,
                                   QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "Failed to register D-Bus object:" << connection.lastError().message();
        return 1;
    }

    qInfo() << "qSnapper D-Bus service started";

    return app.exec();
}
