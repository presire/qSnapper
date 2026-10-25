#ifndef SNAPPERSERVICE_H
#define SNAPPERSERVICE_H

/**
 * @file snapperservice.h
 * @brief QMLから利用するスナップショット操作の窓口となるサービスクラス
 *
 * SnapshotListModelやQMLから呼び出され、D-Bus経由でqsnapper-dbus-serviceへ特権操作を依頼する
 * 結果はシグナルとQ_PROPERTYの変更通知でQML側へ反映し、処理中の状態をoperationBusyで公開する
 *
 * 利用する側 : SnapshotListModel、Main.qml、設定画面とスナップショット一覧ページ
 * 入力 : QMLからのQ_INVOKABLE呼び出し
 * 出力 : Q_PROPERTYの変更通知、各操作の完了シグナル
 * 依存 : Qt D-Bus (com.presire.qsnapper.Operations)、fssnapshot.h
 */

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QMap>
#include <QLoggingCategory>
#include <QDBusPendingCall>
#include <QDBusError>
#include "fssnapshot.h"

Q_DECLARE_LOGGING_CATEGORY(snapperLog)

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
    Q_PROPERTY(bool operationBusy READ operationBusy NOTIFY operationBusyChanged)                           // 作成・変更・削除・ロールバックの処理中かどうか

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
     * @brief 作成・変更・削除・ロールバックのいずれかを処理中かどうかを取得する
     * @return 処理中の操作が1件以上ある場合はtrue
     */
    bool operationBusy() const { return m_pendingOperations > 0; }

    /**
     * @brief 設定一覧を非同期に更新する
     * @note 結果はconfigsChanged()で通知する
     */
    Q_INVOKABLE void refreshConfigs();

    /**
     * @brief 指定設定のスナップショット一覧を非同期に取得する (ListSnapshots)
     * @param configName 対象の設定名
     * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答のQDBusPendingCall
     * @note 応答のCSVはparseSnapshotList()でFsSnapshotのリストへ変換する
     */
    QDBusPendingCall asyncListSnapshots(const QString &configName);

    /**
     * @brief CSV出力を解析してスナップショットリストを生成する
     *
     * 返すオブジェクトは呼び出し側が所有する
     *
     * @param csvOutput qsnapper-dbus-serviceが返したCSV出力
     * @return 呼び出し側が所有するスナップショットリスト
     */
    static QList<FsSnapshot*> parseSnapshotList(const QString &csvOutput);

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
     * @note Polkitは毎回auth_adminで認証を求めるため、認証ダイアログへの入力待ちを見込んで待ち時間を延ばす
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
     * @note DeleteSnapshotの認可 (delete-snapshot) はauth_admin_keepのため、Polkitが認可を一時保持する間は再認証を求めない
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
     *       一時的な拒否は再実行で成功し得るが、認可の拒否は管理者認証が必要であるため、エラー名で判別して理由を翻訳して伝える
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
     * @note 非同期に作成し、結果はsnapshotCreated / snapshotCreationFailedで通知する
     */
    Q_INVOKABLE void createSingle(const QString &description,
                                  FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                 bool important = false,
                                 const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Preスナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @note 非同期に作成し、結果はsnapshotCreated / snapshotCreationFailedで通知する
     */
    Q_INVOKABLE void createPre(const QString &description,
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
     * @note 非同期に作成し、結果はsnapshotCreated / snapshotCreationFailedで通知する
     */
    Q_INVOKABLE void createPost(const QString &description,
                               int previousNumber,
                               FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                               bool important = false,
                               const QVariantMap &userdata = QVariantMap());

    // スナップショット操作 (すべて非同期で、結果はシグナルで通知する)
    /**
     * @brief 指定番号へ非同期にロールバックする
     * @param number ロールバック先のスナップショット番号
     * @note 結果はrollbackCompleted / rollbackFailedで通知する
     */
    Q_INVOKABLE void rollback(int number);

    /**
     * @brief 単一スナップショットを非同期に削除する
     * @param number 削除するスナップショット番号
     * @note 結果はsnapshotDeleted / snapshotDeletionFailedで通知する
     */
    Q_INVOKABLE void deleteSnapshot(int number);

    /**
     * @brief スナップショット情報を非同期に変更する
     * @param number 変更対象のスナップショット番号
     * @param description 新しい説明文
     * @param cleanup 新しいクリーンアップアルゴリズム文字列
     * @param userdata 新しいユーザデータマップ
     * @note 結果はsnapshotModified / snapshotModificationFailedで通知する
     */
    Q_INVOKABLE void modifySnapshot(int number, const QString &description,
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
     * @brief 作成・変更・削除・ロールバックの処理中かどうかが変わったときに発行する
     */
    void operationBusyChanged();

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
     * @brief スナップショット作成の受付可否を確認し、不可なら失敗を通知する
     * @param snapshotType DISABLE_SNAPSHOTSの判定に使う種別文字列
     * @return 作成を続けてよい場合はtrue
     */
    bool checkCreateAllowed(const QString &snapshotType);

    /**
     * @brief スナップショットを非同期に作成する内部ヘルパー
     * @param snapshotType 作成するスナップショット種別
     * @param description 説明文
     * @param previousNumber 紐付け元のPreスナップショット番号
     * @param cleanup クリーンアップアルゴリズム
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     * @param configName 作成先の設定名
     */
    void create(FsSnapshot::SnapshotType snapshotType,
                const QString &description, int previousNumber,
                FsSnapshot::CleanupAlgorithm cleanup,
                bool important,
                const QVariantMap &userdata,
                const QString &configName);

    /**
     * @brief 処理中の操作を1件増やす
     * @note 0件から1件になった場合はoperationBusyChangedを発行する
     */
    void beginOperation();

    /**
     * @brief 処理中の操作を1件減らす
     * @note 1件から0件になった場合はoperationBusyChangedを発行する
     */
    void endOperation();

    // シングルトン
    static SnapperService *s_instance;  // シングルトンインスタンス

    // 状態フラグ
    bool m_configsChecked;          // 設定一覧キャッシュ済みフラグ
    quint64 m_configsGeneration;    // 設定一覧の取得要求ごとに進める世代番号 (古い応答を捨てるために使う)
    int m_pendingOperations;        // 処理中の作成・変更・削除・ロールバックの件数

    // キャッシュ
    QStringList m_configs;          // 利用可能なSnapper設定名のキャッシュ
    QString m_currentConfig;        // 現在選択されている設定名
};

#endif // SNAPPERSERVICE_H
