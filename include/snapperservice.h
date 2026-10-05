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
 * @file snapperservice.h
 * @brief Snapper操作を集約するサービスクラス
 */

/**
 * @class SnapperService
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
    /**
     * @brief SnapperServiceを構築する
     * @param parent Qt親オブジェクト
     */
    explicit SnapperService(QObject *parent = nullptr);

    /**
     * @brief SnapperServiceを破棄する
     */
    ~SnapperService();

    /**
     * @brief シングルトンインスタンスを取得する
     * @return SnapperServiceの唯一のインスタンス
     */
    static SnapperService* instance();

    // 設定状態
    /**
     * @brief Snapperが設定済みか判定する
     * @return 設定済みの場合はtrue
     */
    bool isConfigured();

    /**
     * @brief Snapperを設定する
     */
    Q_INVOKABLE void configureSnapper();

    // 設定 (config) 切替API
    /**
     * @brief 利用可能なSnapper設定一覧を取得する
     * @return 設定名の一覧
     */
    QStringList configs();

    /**
     * @brief 現在選択中の設定名を取得する
     * @return 現在の設定名
     */
    QString currentConfig() const { return m_currentConfig; }

    /**
     * @brief 現在の設定を変更する
     * @param name 新たな設定名
     */
    Q_INVOKABLE void setCurrentConfig(const QString &name);

    /**
     * @brief 設定一覧を更新する
     */
    Q_INVOKABLE void refreshConfigs();

    // 外部ブラウザでURLを開く (QML公開)
    /**
     * @brief URLを外部ブラウザで開く
     * @param url 開くURL
     * @return 開く処理を開始できた場合はtrue
     */
    Q_INVOKABLE bool openExternalUrl(const QString &url);

    // スナップショット作成可否判定
    /**
     * @brief 指定種別のスナップショット作成可否を判定する
     * @param snapshotType 判定するスナップショット種別
     * @return 作成可能な場合はtrue
     */
    Q_INVOKABLE bool createSnapshotAllowed(const QString &snapshotType) const;

    // スナップショット作成 (QML公開)
    /**
     * @brief 単発スナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @return 作成されたスナップショット番号
     */
    Q_INVOKABLE int createSingle(const QString &description,
                                 FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                 bool important = false,
                                 const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Preスナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @return 作成されたスナップショット番号
     */
    Q_INVOKABLE int createPre(const QString &description,
                              FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                              bool important = false,
                              const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Postスナップショットを作成する
     * @param description 説明文
     * @param previousNumber 紐付け元のPreスナップショット番号
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @return 作成されたスナップショット番号
     */
    Q_INVOKABLE int createPost(const QString &description,
                               int previousNumber,
                               FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                               bool important = false,
                               const QVariantMap &userdata = QVariantMap());

    // スナップショット取得・操作
    /**
     * @brief 全スナップショットを取得する
     *
     * 返すオブジェクトはparentを持たず、呼び出し側が所有する
     * all()はリストを返し、QMLの所有権ヒューリスティックはリストの要素には適用されないため、C++からのみ呼ぶ
     *
     * @return 呼び出し側が所有するスナップショットリスト
     */
    QList<FsSnapshot*> all();

    /**
     * @brief 指定番号のスナップショットを検索する
     *
     * find()はQObjectを直接返すため、QMLから呼んだ場合はJavaScriptOwnershipとなりGCで解放される
     * 返すオブジェクトはparentを持たず、呼び出し側が所有する
     *
     * @param number 検索するスナップショット番号
     * @return 呼び出し側が所有するスナップショット、見つからない場合はnullptr
     */
    Q_INVOKABLE FsSnapshot* find(int number);

    /**
     * @brief 指定番号へロールバックする
     * @param number ロールバック先のスナップショット番号
     * @return 成功した場合はtrue
     */
    Q_INVOKABLE bool rollback(int number);

    /**
     * @brief 単一スナップショットを削除する
     * @param number 削除するスナップショット番号
     * @return 成功した場合はtrue
     */
    Q_INVOKABLE bool deleteSnapshot(int number);

    /**
     * @brief スナップショット情報を変更する
     * @param number 変更対象のスナップショット番号
     * @param description 新しい説明文
     * @param cleanup 新しいクリーンアップアルゴリズム文字列
     * @param userdata 新しいユーザデータマップ
     * @return 成功した場合はtrue
     */
    Q_INVOKABLE bool modifySnapshot(int number, const QString &description,
                                    const QString &cleanup, const QVariantMap &userdata);

    // インストール時設定
    /**
     * @brief インストール時設定フラグを設定する
     * @param value 設定するフラグ値
     */
    void setConfigureOnInstall(bool value) { m_configureOnInstall = value; }

    /**
     * @brief インストール時設定フラグを取得する
     * @return インストール時に設定を行う場合はtrue
     */
    bool configureOnInstall() const { return m_configureOnInstall; }

signals:
    /**
     * @brief 設定状態変更時に発行する
     * @param configured 設定済みかどうか
     */
    void configuredChanged(bool configured);

    /**
     * @brief 設定一覧変更時に発行する
     */
    void configsChanged();

    /**
     * @brief 現在設定変更時に発行する
     */
    void currentConfigChanged();

    /**
     * @brief スナップショット作成成功時に発行する
     * @param snapshot 作成されたスナップショット (所有者はSnapperService、発行中のみ有効)
     */
    void snapshotCreated(FsSnapshot *snapshot);

    /**
     * @brief スナップショット作成失敗時に発行する
     * @param error 失敗理由
     */
    void snapshotCreationFailed(const QString &error);

    /**
     * @brief ロールバック完了時に発行する
     */
    void rollbackCompleted();

    /**
     * @brief ロールバック失敗時に発行する
     * @param error 失敗理由
     */
    void rollbackFailed(const QString &error);

    /**
     * @brief 削除成功時に発行する
     * @param number 削除されたスナップショット番号
     */
    void snapshotDeleted(int number);

    /**
     * @brief 削除失敗時に発行する
     * @param number 削除対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotDeletionFailed(int number, const QString &error);

    /**
     * @brief 変更成功時に発行する
     * @param number 変更されたスナップショット番号
     */
    void snapshotModified(int number);

    /**
     * @brief 変更失敗時に発行する
     * @param number 変更対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotModificationFailed(int number, const QString &error);

private:
    // 内部作成ヘルパー
    /**
     * @brief スナップショットを作成する内部ヘルパー
     * @param snapshotType 作成するスナップショット種別
     * @param description 説明文
     * @param previousNumber 紐付け元のPreスナップショット番号
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @return 作成されたスナップショット番号
     */
    int create(FsSnapshot::SnapshotType snapshotType,
               const QString &description, int previousNumber = -1,
               FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
               bool important = false,
               const QVariantMap &userdata = QVariantMap());

    // パス・環境判定
    /**
     * @brief ターゲットルートパスを取得する
     * @return スナップショット対象のルートパス
     */
    QString targetRoot() const;

    /**
     * @brief 非切り替えインストールか判定する
     * @return 非切り替えインストールの場合はtrue
     */
    bool nonSwitchedInstallation() const;

    // インストール補助
    /**
     * @brief Snapper設定ファイルを書き込む
     */
    void writeSnapperConfig();

    /**
     * @brief Snapperクォータを設定する
     */
    void setupSnapperQuota();

    // 解析・実行
    /**
     * @brief CSV出力を解析してスナップショットリストを生成する
     *
     * 返すオブジェクトは呼び出し側が所有する
     *
     * @param csvOutput Snapperが返したCSV出力
     * @return 呼び出し側が所有するスナップショットリスト
     */
    QList<FsSnapshot*> parseSnapshotList(const QString &csvOutput);

    /**
     * @brief D-Busサービスへ再接続する
     * @return 接続に成功した場合はtrue
     */
    bool reconnect();

    // シングルトン
    static SnapperService *s_instance;  // シングルトンインスタンス

    // 状態フラグ
    bool m_configured;          // Snapperが設定済みかどうか
    bool m_configuredChecked;   // 設定チェック済みフラグ
    bool m_configureOnInstall;  // インストール時に設定を行うかどうか
    bool m_configsChecked;      // 設定一覧キャッシュ済みフラグ

    // 通信・キャッシュ
    QDBusInterface *m_dbusInterface;    // D-Bus通信インターフェース
    QStringList m_configs;              // 利用可能なSnapper設定名のキャッシュ
    QString m_currentConfig;            // 現在選択されている設定名
};

#endif // SNAPPERSERVICE_H
