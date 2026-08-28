#include "singleinstanceguard.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QtGlobal>
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
 * lock fileパスを決定し、stale lock判定時間を設定する
 */
SingleInstanceGuard::SingleInstanceGuard(QObject *parent)
    : QObject(parent)
    , m_server(nullptr)
    , m_lockFile(new QLockFile(lockFilePath()))
{
    m_lockFile->setStaleLockTime(30000);
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
 * @brief UIDごとに分離されたローカルサーバ名を返す
 *
 * マルチユーザ環境でもユーザ単位で独立した単一インスタンス制御を行う
 */
QString SingleInstanceGuard::serverName() const
{
    // UID単位で分離することで、マルチユーザ環境では各ユーザが独立に1インスタンスずつ起動可能とする
    return QStringLiteral("qsnapper-%1").arg(::getuid());
}

/**
 * @brief lock fileの配置パスを返す
 *
 * 通常はXDG_RUNTIME_DIRを使い、未設定環境では /tmp にフォールバックする
 */
QString SingleInstanceGuard::lockFilePath() const
{
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        // RuntimeLocationが使えない環境では /tmp フォールバックを許容する
        // これはroot権限境界ではなく、GUIクライアント側の単一インスタンス制御であり、
        // lock名もUID単位で分離されるため、ここではgraceful degradationを優先する
        // ただし /tmp は本来の理想配置ではないため、通常運用ではXDG_RUNTIME_DIRを優先する
        runtimeDir = QDir::tempPath();
    }
    return runtimeDir + QLatin1Char('/') + serverName() + QStringLiteral(".lock");
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
 * @brief プライマリインスタンス取得を試みる
 *
 * 既存インスタンスがいればraise要求を送信し、自身はセカンダリとしてfalseを返す
 * 既存インスタンスがいなければlistenを開始し、プライマリとしてtrueを返す
 */
bool SingleInstanceGuard::tryAcquire()
{
    const QString name = serverName();

    // 既存インスタンスへの接続を試みる
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(kConnectTimeoutMs)) {
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

    // 接続に失敗 = 既存インスタンスなし、または前回クラッシュ等でソケットファイルが残留している可能性がある
    if (!tryAcquireLock()) {
        qWarning() << "SingleInstanceGuard: failed to acquire instance lock:" << lockFilePath();
        return false;
    }

    // removeServer() はstaleなソケットを安全に削除する
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
