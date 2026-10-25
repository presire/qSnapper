#ifndef SNAPPERSERVICE_H
#define SNAPPERSERVICE_H

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QMap>
#include <QLoggingCategory>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusError>
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

    // 設定画面向けの非同期D-Bus呼び出し (C++専用)
    // 設定画面はGUIスレッドをブロックしないよう、同期呼び出しを使わずQDBusPendingCallで結果を受け取る
    // 呼び出し元はQDBusPendingCallWatcherを作成して完了を待つ
    // reconnect()は同期呼び出しでGUIを止めるため使わず、特権サービスの起動はバスのD-Bus activationに任せる
    /**
     * @brief 指定設定のsnapper設定値を非同期に取得する (GetConfig)
     * @param configName 取得対象の設定名
     * @return 応答がa{ss}のQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     */
    QDBusPendingCall asyncGetConfig(const QString &configName);

    /**
     * @brief 指定設定のsnapper設定値を非同期に書き込む (WriteSnapperConfig)
     * @param configName 書き込み対象の設定名
     * @param settings 書き込む設定値 (サーバ側が許可する編集可能キーだけを渡す)
     * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     */
    QDBusPendingCall asyncWriteSnapperConfig(const QString &configName, const QMap<QString, QString> &settings);

    /**
     * @brief 指定設定のBtrfsクォータを非同期に設定する (SetupQuota)
     * @param configName 対象の設定名
     * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     */
    QDBusPendingCall asyncSetupQuota(const QString &configName);

    /**
     * @brief snapperのsystemdタイマの状態を非同期に取得する (GetTimerStates)
     * @return 応答がa{s(ss)}のQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     */
    QDBusPendingCall asyncGetTimerStates();

    /**
     * @brief snapperのsystemdタイマの有効化と無効化を非同期に要求する (SetTimerEnabled)
     * @param unit 対象のユニット名 (snapper-timeline.timerなど)
     * @param enabled trueで有効化して起動、falseで停止して無効化
     * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     */
    QDBusPendingCall asyncSetTimerEnabled(const QString &unit, bool enabled);

    /**
     * @brief 設定の作成を非同期に要求する (CreateConfig)
     * @param configName 作成する設定名 (サーバ側でも同じ規則で検証される)
     * @param subvolume 設定が対象とするBtrfsサブボリュームの絶対パス
     * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
     * @note polkitは毎回auth_adminで認証を求めるため、認証ダイアログへの入力待ちを見込んで待ち時間を延ばす
     *       サーバ側がfstype=btrfsとtemplate=defaultを固定する
     */
    QDBusPendingCall asyncCreateConfig(const QString &configName, const QString &subvolume);

    /**
     * @brief 設定の削除を非同期に要求する (DeleteConfig)
     * @param configName 削除する設定名
     * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
     * @note 対象設定の全スナップショットも削除されるため、完了までに時間がかかる
     *       サーバ側はSUBVOLUMEが/の設定 (システムのルート) を拒否する
     */
    QDBusPendingCall asyncDeleteConfig(const QString &configName);

    /**
     * @brief 複数スナップショットの一括削除を非同期に要求する (DeleteSnapshots)
     * @param configName 対象の設定名
     * @param numbers 削除するスナップショット番号 (1以上、重複なし)
     * @return 応答がauのQDBusPendingCall、接続できない場合はエラー応答
     * @note 認証は1回だけ求められるため、番号ごとの個別呼び出しよりプロンプトが少なくなる
     *       応答のリストは再読み込み後に削除を確認できた番号であり、要求より短い場合は一部失敗を意味する
     */
    QDBusPendingCall asyncDeleteSnapshots(const QString &configName, const QList<uint> &numbers);

    /**
     * @brief 1件のスナップショットの削除を、ヘルパのDeleteSnapshotメソッドへ非同期に要求する
     * @param configName 削除するスナップショットが属するSnapperの設定名
     * @param number 削除するスナップショット番号 (1以上)
     * @return 応答の値がboolになるQDBusPendingCall
     *         ヘルパへ接続できない場合は、エラー応答を持つQDBusPendingCall
     * @note DeleteSnapshotの認可 (delete-snapshot) はauth_admin_keepのため、polkitが認可を一時保持する間は再認証を求めない
     *       ヘルパはsnapperdを経由せずlibsnapperで直接削除する
     *       そのため、snapperdが保持する古い一覧が原因の拒否 (SnapshotListOutdated) は発生しない
     */
    QDBusPendingCall asyncDeleteSnapshot(const QString &configName, uint number);

    /**
     * @brief 設定のファイルシステム使用量を非同期に取得する (GetFilesystemUsage)
     * @param configName 対象の設定名
     * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答
     * @note 認可はallow_activeのため認証は求められないが、サーバ側の処理時間を見込んで待ち時間を延ばす
     */
    QDBusPendingCall asyncGetFilesystemUsage(const QString &configName);

    /**
     * @brief 設定のサブボリューム一覧を非同期に取得する (ListSubvolumes)
     * @param configName 対象の設定名
     * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答
     * @note 認可はauth_admin_keepのため、初回は認証ダイアログへの入力待ちが発生する
     */
    QDBusPendingCall asyncListSubvolumes(const QString &configName);

    /**
     * @brief D-Busエラーを利用者へ表示する文言へ変換する
     * @param error 特権サービスから返されたD-Busエラー
     * @return 既知の拒否理由であれば翻訳済みの固定文言、それ以外はエラー本文
     * @note エラー本文は特権サービスが返す固定の英語文言であり、設定ファイルの内容は含まない
     *       拒否は一時的なものが多く、完了後に再実行すれば成功するため、エラー名で判別して理由を翻訳して伝える
     *       エラー名はdbus/com.presire.qsnapper.Operations.xmlに記載した名前と一致させる必要がある
     */
    static QString errorText(const QDBusError &error);

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

signals:
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
    bool m_configsChecked;      // 設定一覧キャッシュ済みフラグ

    // 通信・キャッシュ
    QDBusInterface *m_dbusInterface;    // D-Bus通信インターフェース
    QStringList m_configs;              // 利用可能なSnapper設定名のキャッシュ
    QString m_currentConfig;            // 現在選択されている設定名
};

#endif // SNAPPERSERVICE_H
