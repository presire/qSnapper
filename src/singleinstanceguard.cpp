#include "singleinstanceguard.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QDebug>
#include <QtGlobal>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    /// @brief 既存インスタンスへの接続試行のタイムアウト (ミリ秒)
    constexpr int kConnectTimeoutMs = 500;

    /// @brief raise要求メッセージ (プロトコルが単純なため固定文字列)
    constexpr const char *kRaiseMessage = "RAISE\n";
}

/**
 * @brief SingleInstanceGuardを初期化する
 *
 * ユーザ専用のruntimeディレクトリを決定し、使える場合だけlock fileを用意する
 */
SingleInstanceGuard::SingleInstanceGuard(QObject *parent)
    : QObject(parent)
    , m_server(nullptr)
    , m_runtimeDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
{
    if (!m_runtimeDir.isEmpty()) {
        m_lockFile = std::make_unique<QLockFile>(lockFilePath());
        // lockは起動中ずっと保持するため、経過時間ではstaleと判定しない (保持プロセスの生存で判定する)
        m_lockFile->setStaleLockTime(0);
    }
}

/**
 * @brief 単一インスタンス関連のリソースを解放する
 *
 * listen中のローカルサーバを閉じ、保持しているlockを解除する
 */
SingleInstanceGuard::~SingleInstanceGuard()
{
    if (m_server) {
        m_server->close();
    }

    if (m_lockFile) {
        m_lockFile->unlock();
    }
}

/**
 * @brief ローカルサーバのソケットパスを返す
 *
 * ユーザ専用のruntimeディレクトリ配下の絶対パスとする
 * 相対名にすると、Qtは共有の /tmp にソケットを作るため、他のユーザが同名のソケットを先に作れてしまう
 */
QString SingleInstanceGuard::serverName() const
{
    return m_runtimeDir + QStringLiteral("/qsnapper.socket");
}

/**
 * @brief lock fileの配置パスを返す
 *
 * ソケットと同じく、ユーザ専用のruntimeディレクトリ配下に置く
 */
QString SingleInstanceGuard::lockFilePath() const
{
    return m_runtimeDir + QStringLiteral("/qsnapper.lock");
}

/**
 * @brief 補助lock fileの取得を試みる
 *
 * stale lockが見つかった場合は削除後に再取得を試みる
 */
bool SingleInstanceGuard::tryAcquireLock()
{
    if (m_lockFile && m_lockFile->tryLock()) {
        return true;
    }

    if (m_lockFile && m_lockFile->removeStaleLockFile()) {
        return m_lockFile->tryLock();
    }

    return false;
}

/**
 * @brief 接続先のサーバが自分と同じUIDのプロセスかを確認する
 *
 * @param socket 接続済みのソケット
 * @return 同じUIDの場合: true、異なる・確認できない場合: false
 */
bool SingleInstanceGuard::isPeerSameUser(const QLocalSocket &socket)
{
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(static_cast<int>(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                     &credentials, &length) < 0 || length != sizeof(credentials)) {
        return false;
    }
    return credentials.uid == ::getuid();
}

/**
 * @brief プライマリインスタンス取得を試みる
 *
 * 既存インスタンスがいればraise要求を送信し、自身はセカンダリとしてfalseを返す
 * 既存インスタンスがいなければlistenを開始し、プライマリとしてtrueを返す
 * ユーザ専用のruntimeディレクトリを使えない場合は、二重起動防止を無効にして起動を許容する
 */
bool SingleInstanceGuard::tryAcquire()
{
    if (m_runtimeDir.isEmpty()) {
        // 共有の /tmp には置かない (他のユーザが先にソケットやlockを作り、起動を妨げられるため)
        qWarning() << "SingleInstanceGuard: runtime directory is unavailable; single-instance guard is disabled";
        return true;
    }

    const QString name = serverName();

    // 既存インスタンスへの接続を試みる
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(kConnectTimeoutMs)) {
        if (!isPeerSameUser(probe)) {
            // 自分以外のプロセスが待ち受けている場合は、既存インスタンスとして扱わない
            qWarning() << "SingleInstanceGuard: ignoring a server owned by another user";
            probe.abort();
        }
        else {
            // 既存インスタンスにraise要求を送信
            const qint64 expectedBytes = qstrlen(kRaiseMessage);
            const qint64 writtenBytes = probe.write(kRaiseMessage);

            if (writtenBytes != expectedBytes) {
                qWarning() << "SingleInstanceGuard: failed to queue raise request:"
                           << probe.errorString();
            }
            else if (!probe.waitForBytesWritten(kConnectTimeoutMs)) {
                qWarning() << "SingleInstanceGuard: failed to deliver raise request:"
                           << probe.errorString();
            }

            probe.disconnectFromServer();
            return false;
        }
    }

    // 接続に失敗 = 既存インスタンスなし、または前回クラッシュ等でソケットファイルが残留している可能性がある
    if (!tryAcquireLock()) {
        qWarning() << "SingleInstanceGuard: failed to acquire instance lock";
        return false;
    }

    // lockを保持しているため、残っているソケットは前回のものであり、削除してよい
    QLocalServer::removeServer(name);

    m_server = new QLocalServer(this);
    // ソケットパーミッションは本ユーザのみに制限 (他UIDからの偽装防止)
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    if (!m_server->listen(name)) {
        qWarning() << "SingleInstanceGuard: listen failed:"
                   << m_server->errorString();
        if (m_lockFile) {
            m_lockFile->unlock();
        }
        // listen失敗でも起動は許容する (ガード機能のみ無効化)
        return true;
    }

    connect(m_server, &QLocalServer::newConnection, this, &SingleInstanceGuard::onNewConnection);

    return true;
}

/**
 * @brief 既存インスタンスへの新規接続を処理する
 *
 * 現状のプロトコルはraise要求のみを想定しており、接続受理時に raiseRequested() を通知する
 */
void SingleInstanceGuard::onNewConnection()
{
    while (QLocalSocket *client = m_server->nextPendingConnection()) {
        // 受信データは現状 raise 要求のみなので内容は検証せず破棄
        // 将来コマンドを増やす場合はここでパースする
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        emit raiseRequested();
    }
}
