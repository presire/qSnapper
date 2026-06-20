#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QMap>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "snapshotoperations.h"
#include "filesystemhelpers.h"

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

namespace {

bool ensureRealLogDirectory()
{
    if (!qsnapper::security::safeMkpath(logDir(), 0700)) {
        return false;
    }

    struct stat st;
    return qsnapper::security::safeLstat(logDir(), &st) && S_ISDIR(st.st_mode);
}

int openLogFileNoFollow()
{
    const QByteArray logFilePath = logFile().toUtf8();
    const int fd = ::open(logFilePath.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }

    if ((st.st_mode & 0777) != 0600 && ::fchmod(fd, 0600) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

}

static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    // ログディレクトリを作成し、パーミッションを0700にする
    // root:rootで起動されるため、所有者はrootに固定される
    // 恒久的なmode設定はsystemd-tmpfiles (tmpfiles.d/qsnapper.conf) 側で担保するが、
    // パッケージ導入前の初回起動でも安全側に倒すため本関数でも設定する
    if (!ensureRealLogDirectory()) {
        return;
    }

    const int fd = openLogFileNoFollow();
    if (fd < 0) {
        return;
    }

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

    const QString logLine = QDateTime::currentDateTime().toString(Qt::ISODate)
            + QStringLiteral(" [") + QString::fromLatin1(level) + QStringLiteral("] ")
            + msg + QLatin1Char('\n');
    const QByteArray encoded = logLine.toUtf8();

    qsizetype offset = 0;
    while (offset < encoded.size()) {
        const ssize_t written = ::write(fd, encoded.constData() + offset,
                                        static_cast<size_t>(encoded.size() - offset));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        offset += written;
    }

    ::close(fd);
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(fileMessageHandler);

    // QMap<QString,QString> を D-Bus a{ss} としてマーシャリングするために登録
    qDBusRegisterMetaType<QMap<QString, QString>>();

    QCoreApplication app(argc, argv);
    app.setOrganizationName("Presire");
    app.setApplicationName("qSnapper D-Bus Service");
    app.setApplicationVersion(QSNAPPER_VERSION);

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
