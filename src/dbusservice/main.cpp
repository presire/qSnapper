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

namespace {

/**
 * @brief ログファイルを1回だけ開き、ディスクリプタを返す
 *
 * ログディレクトリをsymlink非追従で作成・オープンし、root所有かつgroup/otherから書き込めないことを確認してから、
 * そのdirfd相対でログファイルをO_NOFOLLOWで開く
 * 開いたディスクリプタはプロセス終了まで保持し、メッセージごとのパス解決は行わない
 * サービスはアイドル時に終了するため、ログのローテーション後も次回起動時に新しいファイルが開かれる
 *
 * @return 成功時はfile descriptor、失敗時は-1 (失敗後は再試行しない)
 */
int openLogFileOnce()
{
    // root:rootで起動されるため、作成されるディレクトリの所有者はrootになる
    // 恒久的なmode設定はsystemd-tmpfiles (tmpfiles.d/qsnapper.conf) 側で担保するが、
    // パッケージ導入前の初回起動でも安全側に倒すため本関数でも0700で作成する
    if (!qsnapper::security::safeMkpath(logDir(), 0700)) {
        return -1;
    }

    const int dirFd = qsnapper::security::safeOpenDirectory(logDir());
    if (dirFd < 0) {
        return -1;
    }

    struct stat dirSt;
    if (::fstat(dirFd, &dirSt) != 0 || !S_ISDIR(dirSt.st_mode)
        || dirSt.st_uid != ::geteuid() || (dirSt.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        ::close(dirFd);
        return -1;
    }

    const int fd = ::openat(dirFd, "qsnapper-dbus.log",
                            O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY, 0600);
    ::close(dirFd);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) {
        ::close(fd);
        return -1;
    }

    if ((st.st_mode & 0777) != 0600 && ::fchmod(fd, 0600) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief 保持しているログファイルのディスクリプタを返す
 *
 * 初回呼び出し時にのみopenLogFileOnce()を実行する (関数内staticの初期化はスレッドセーフ)
 */
int logFileDescriptor()
{
    static const int fd = openLogFileOnce();
    return fd;
}

/**
 * @brief 1件のログメッセージを1行に収めるため、制御文字を16進表記 (\\xHH) にエスケープする
 *
 * const char*で渡された例外メッセージ等に改行が含まれていても、ログの行を偽造できないようにする
 */
QString escapeControlCharacters(const QString &msg)
{
    QString escaped;
    escaped.reserve(msg.size());
    for (const QChar ch : msg) {
        const char16_t code = ch.unicode();
        if (code < 0x20 || code == 0x7f || (code >= 0x80 && code < 0xa0)) {
            escaped += QStringLiteral("\\x%1").arg(static_cast<uint>(code), 2, 16, QLatin1Char('0'));
        }
        else {
            escaped += ch;
        }
    }

    return escaped;
}

}

static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    const int fd = logFileDescriptor();
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
            + escapeControlCharacters(msg) + QLatin1Char('\n');
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

    // オブジェクトを作成して登録 (シグナルもエクスポート)
    // サービス名を取得した時点で活性化を待っていた呼び出しが配送されるため、
    // オブジェクトを先に登録し、サービス名の取得は最後に行う
    SnapshotOperations operations;
    if (!connection.registerObject("/com/presire/qsnapper/Operations", &operations,
                                   QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "Failed to register D-Bus object:" << connection.lastError().message();
        return 1;
    }

    // サービスを登録
    if (!connection.registerService("com.presire.qsnapper.Operations")) {
        qCritical() << "Failed to register D-Bus service:" << connection.lastError().message();
        return 1;
    }

    qInfo() << "qSnapper D-Bus service started";

    return app.exec();
}
