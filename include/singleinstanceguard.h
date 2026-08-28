#ifndef SINGLEINSTANCEGUARD_H
#define SINGLEINSTANCEGUARD_H

#include <QObject>
#include <QString>
#include <memory>

class QLocalServer;
class QLockFile;

/**
 * @brief 二重起動を防止するためのガードクラス
 *
 * QLocalServer/QLocalSocketを用いて、UID単位でアプリケーションの単一インスタンスを保証する
 * 既に別インスタンスが起動している場合、そのインスタンスに対してraise要求を送信してから終了できる
 *
 * 使用手順:
 * 1. tryAcquire()を呼び出す
 * 2. 戻り値がfalseの場合、既存インスタンスが存在するので終了する
 * 3. 戻り値がtrueの場合、本インスタンスがプライマリとなる
 *    raiseRequested()シグナルをウィンドウの前面化処理へ接続する
 *
 * サーバ名は "qsnapper-<UID>" 形式で、マルチユーザ環境でもユーザごとに独立して1インスタンスを起動可能
 *
 * @note 通常は XDG_RUNTIME_DIR 配下に lock を置く。未設定環境では /tmp にフォールバックするが、
 *       この分岐は、GUI側の利便性確保を目的とした劣化運転であり、詳細な前提は実装側コメントを参照
 */
class SingleInstanceGuard : public QObject
{
    Q_OBJECT

private:
    // パス生成
    QString serverName() const;             // サーバ名を生成する
    QString lockFilePath() const;           // ロックファイルパスを生成する
    bool tryAcquireLock();                  // ロック取得を試みる

    // リソース
    QLocalServer *m_server;                 // 単一インスタンス用ローカルサーバ
    std::unique_ptr<QLockFile> m_lockFile;  // 単一インスタンス用ロックファイル

public:
    // コンストラクタ/デストラクタ
    explicit SingleInstanceGuard(QObject *parent = nullptr);    // コンストラクタ
    ~SingleInstanceGuard() override;                            // デストラクタ

    /**
     * @brief プライマリインスタンスの取得を試みる
     *
     * 既存インスタンスが存在する場合は、そのインスタンスへraise要求を送信してfalseを返す
     * 存在しない場合は自身がプライマリとなり、QLocalServerの待ち受けを開始してtrueを返す
     *
     * @return プライマリ取得成功時: true、既存インスタンスあり: false
     */
    bool tryAcquire(); // プライマリインスタンス取得を試みる

signals:
    /**
     * @brief 別プロセスからの起動要求を受信したときに発火
     *
     * main.cppでルートウィンドウのshow() / raise() / requestActivate()に接続することを想定している
     */
    void raiseRequested();  // 起動要求受信時に発行する

private slots:
    void onNewConnection(); // 新規接続を処理する
};

#endif // SINGLEINSTANCEGUARD_H
