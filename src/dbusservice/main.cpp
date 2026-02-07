#include "snapshotoperations.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

static const QString &logDir()
{
    static const QString dir = QStringLiteral(QSNAPPER_LOG_DIR);
    return dir;
}

static const QString &logFile()
{
    static const QString file = logDir() + QStringLiteral("/qsnapper-dbus.log");
    return file;
}

static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    QDir().mkpath(logDir());

    QFile file(logFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    const char *level = nullptr;
    switch (type) {
        case QtDebugMsg:
            level = "DEBUG";
            break;
        case QtInfoMsg:
            level = "INFO";
            break;
        case QtWarningMsg:
            level = "WARNING";
            break;
        case QtCriticalMsg:
            level = "CRITICAL";
            break;
        case QtFatalMsg:
            level = "FATAL";
            break;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODate)
           << " [" << level << "] "
           << msg << "\n";
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(fileMessageHandler);

    QCoreApplication app(argc, argv);
    app.setOrganizationName("Presire");
    app.setApplicationName("qSnapper D-Bus Service");
    app.setApplicationVersion("1.0.2");

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
