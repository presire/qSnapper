#ifndef SNAPPERSERVICE_H
#define SNAPPERSERVICE_H

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QLoggingCategory>
#include <QDBusInterface>
#include "fssnapshot.h"

Q_DECLARE_LOGGING_CATEGORY(snapperLog)

/**
 * @brief Snapper操作を集約するサービスクラス
 *
 * D-Bus経由でSnapperデーモンと連携し、スナップショットの作成・取得・削除・ロールバック・変更および設定管理を提供するシングルトンサービス
 * QMLからはSnapshotListModel経由または直接呼び出しで利用される
 */
class SnapperService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ isConfigured NOTIFY configuredChanged)                                  // Snapper設定済みフラグ
    Q_PROPERTY(QStringList configs READ configs NOTIFY configsChanged)                                      // 利用可能な設定一覧
    Q_PROPERTY(QString currentConfig READ currentConfig WRITE setCurrentConfig NOTIFY currentConfigChanged) // 現在選択中の設定名

public:
    // コンストラクタ/デストラクタ/シングルトン
    explicit SnapperService(QObject *parent = nullptr);         // コンストラクタ
    ~SnapperService();                                          // デストラクタ
    static SnapperService* instance();                          // シングルトンインスタンスを取得する

    // 設定状態
    bool isConfigured();                                        // Snapperが設定済みか判定する
    Q_INVOKABLE void configureSnapper();                        // Snapperを設定する

    // 設定 (config) 切替API
    QStringList configs();                                      // 利用可能なSnapper設定一覧を取得する
    QString currentConfig() const { return m_currentConfig; }   // 現在選択中の設定名を取得する
    Q_INVOKABLE void setCurrentConfig(const QString &name);     // 現在の設定を変更する
    Q_INVOKABLE void refreshConfigs();                          // 設定一覧を更新する

    // スナップショット作成可否判定
    Q_INVOKABLE bool createSnapshotAllowed(const QString &snapshotType) const;  // 指定種別のスナップショット作成可否を判定する

    // スナップショット作成 (QML公開)
    Q_INVOKABLE FsSnapshot* createSingle(const QString &description,            // 単発スナップショットを作成する
                                         FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                         bool important = false,
                                         const QVariantMap &userdata = QVariantMap());
    Q_INVOKABLE FsSnapshot* createPre(const QString &description,               // Preスナップショットを作成する
                                      FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                      bool important = false,
                                      const QVariantMap &userdata = QVariantMap());
    Q_INVOKABLE FsSnapshot* createPost(const QString &description,              // Postスナップショットを作成する
                                       int previousNumber,
                                       FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                       bool important = false,
                                       const QVariantMap &userdata = QVariantMap());

    // スナップショット取得・操作
    Q_INVOKABLE QList<FsSnapshot*> all();                                       // 全スナップショットを取得する
    Q_INVOKABLE FsSnapshot* find(int number);                                   // 指定番号のスナップショットを検索する
    Q_INVOKABLE bool rollback(int number);                                      // 指定番号へロールバックする
    Q_INVOKABLE bool deleteSnapshot(int number);                                // 単一スナップショットを削除する
    Q_INVOKABLE bool modifySnapshot(int number, const QString &description,     // スナップショット情報を変更する
                                    const QString &cleanup, const QVariantMap &userdata);

    // インストール時設定
    void setConfigureOnInstall(bool value) { m_configureOnInstall = value; }    // インストール時設定フラグを設定する
    bool configureOnInstall() const { return m_configureOnInstall; }            // インストール時設定フラグを取得する

signals:
    void configuredChanged(bool configured);                            // 設定状態変更時に発行する
    void configsChanged();                                              // 設定一覧変更時に発行する
    void currentConfigChanged();                                        // 現在設定変更時に発行する
    void snapshotCreated(FsSnapshot *snapshot);                         // スナップショット作成成功時に発行する
    void snapshotCreationFailed(const QString &error);                  // スナップショット作成失敗時に発行する
    void rollbackCompleted();                                           // ロールバック完了時に発行する
    void rollbackFailed(const QString &error);                          // ロールバック失敗時に発行する
    void snapshotDeleted(int number);                                   // 削除成功時に発行する
    void snapshotDeletionFailed(int number, const QString &error);      // 削除失敗時に発行する
    void snapshotModified(int number);                                  // 変更成功時に発行する
    void snapshotModificationFailed(int number, const QString &error);  // 変更失敗時に発行する

private:
    // 内部作成ヘルパー
    FsSnapshot* create(FsSnapshot::SnapshotType snapshotType,           // スナップショットを作成する内部ヘルパー
                       const QString &description, FsSnapshot *previous = nullptr,
                       FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                       bool important = false,
                       const QVariantMap &userdata = QVariantMap());

    // パス・環境判定
    QString targetRoot() const;                                     // ターゲットルートパスを取得する
    bool nonSwitchedInstallation() const;                           // 非切り替えインストールか判定する

    // インストール補助
    void installationHelperStep4();                                 // インストールヘルパー手順4を実行する
    void writeSnapperConfig();                                      // Snapper設定ファイルを書き込む
    void updateEtcSysconfigYast2();                                 // /etc/sysconfig/yast2を更新する
    void setupSnapperQuota();                                       // Snapperクォータを設定する

    // 解析・実行
    QList<FsSnapshot*> parseSnapshotList(const QString &csvOutput); // CSV出力を解析してスナップショットリストを生成する
    QString executeCommand(const QString &program,                  // 外部コマンドを実行する
                           const QStringList &arguments,
                           bool &success);
    bool reconnect();                                               // D-Busサービスへ再接続する
    static SnapperService *s_instance;                              // シングルトンインスタンス

    // 状態フラグ
    bool m_configured;                                              // Snapperが設定済みかどうか
    bool m_configuredChecked;                                       // 設定チェック済みフラグ
    bool m_configureOnInstall;                                      // インストール時に設定を行うかどうか
    bool m_configsChecked;                                          // 設定一覧キャッシュ済みフラグ

    // 通信・キャッシュ
    QDBusInterface *m_dbusInterface;                                // D-Bus通信インターフェース
    QStringList m_configs;                                          // 利用可能なSnapper設定名のキャッシュ
    QString m_currentConfig;                                        // 現在選択されている設定名
};

#endif // SNAPPERSERVICE_H
