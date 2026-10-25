/**
 * @file snapperservice.cpp
 * @brief Snapper操作サービスのD-Bus連携実装
 */

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCall>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusMetaType>
#include <QMap>
#include <QDebug>
#include <memory>
#include "snapperservice.h"
#include "snappertimerstate.h"
#include "csvrecord.h"
#include "externalurlopener.h"

// QVariantMap --> QMap<QString,QString> 変換ヘルパー
/**
 * @brief QVariantMapをD-Bus送信用のQMap<QString,QString>へ変換する
 * @param src 変換元のマップ
 * @return 各値を文字列化したマップ
 */
static QMap<QString, QString> toStringMap(const QVariantMap &src)
{
    QMap<QString, QString> out;
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        out.insert(it.key(), it.value().toString());
    }
    return out;
}

Q_LOGGING_CATEGORY(snapperLog, "qsnapper")

SnapperService* SnapperService::s_instance = nullptr;

/**
 * @brief D-Busエラーを利用者へ表示する文言へ変換する
 * @param error ヘルパから返されたD-Busエラー
 * @return 既知の拒否理由であれば翻訳済みの固定文言、それ以外はエラー本文
 * @note ヘルパは復元や設定変更の実行中に操作を一律で拒否し、英語の固定文言だけを返す
 *       拒否は一時的なものが多く、完了後に再実行すれば成功するため、エラー名で判別して理由を翻訳して伝える
 *       エラー名はdbus/com.presire.qsnapper.Operations.xmlに記載した名前と一致させる必要がある
 */
QString SnapperService::errorText(const QDBusError &error)
{
    const QString name = error.name();
    if (name == QLatin1String("com.presire.qsnapper.Error.RestoreInProgress")) {
        return QCoreApplication::translate("SnapperService",
                                           "This operation cannot be performed while a restore is in progress. Try again after the restore finishes.");
    }
    if (name == QLatin1String("com.presire.qsnapper.Error.OperationInProgress")) {
        return QCoreApplication::translate("SnapperService",
                                           "Another configuration or bulk delete operation is in progress.");
    }
    if (name == QLatin1String("com.presire.qsnapper.Error.OutcomeUnknown")) {
        return QCoreApplication::translate("SnapperService",
                                           "The operation did not finish in time. Reload to see the current state.");
    }
    if (name == QLatin1String("com.presire.qsnapper.Error.RestorePlanPending")) {
        return QCoreApplication::translate("SnapperService",
                                           "A restore is being prepared for this configuration.");
    }
    if (name == QLatin1String("com.presire.qsnapper.Error.SnapshotListOutdated")) {
        return QCoreApplication::translate("SnapperService",
                                           "The snapshot list is being updated by Snapper. Try again in a minute.");
    }
    return error.message();
}

/**
 * @brief SnapperServiceのコンストラクタ
 *
 * D-Busインターフェースを初期化し、Snapperサービスへの接続を確立する
 *
 * @param parent 親オブジェクト
 */
SnapperService::SnapperService(QObject *parent)
    : QObject(parent)
    , m_dbusInterface(nullptr)
    , m_configsChecked(false)
    , m_currentConfig(QStringLiteral("root"))
{
    // QMap<QString,QString> の D-Bus 型登録 (クライアント側)
    qDBusRegisterMetaType<QMap<QString, QString>>();

    // GetTimerStates の応答 a{s(ss)} を復元するための型登録 (構造体 (ss) と、そのマップ)
    qDBusRegisterMetaType<SnapperTimerState>();
    qDBusRegisterMetaType<SnapperTimerStateMap>();

    m_dbusInterface = new QDBusInterface(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        QDBusConnection::systemBus(),
        this
    );

    if (!m_dbusInterface->isValid()) {
        qCWarning(snapperLog) << "Failed to connect to D-Bus service:"
                              << QDBusConnection::systemBus().lastError().message();
    }
}

/**
 * @brief SnapperServiceのデストラクタ
 *
 * リソースのクリーンアップを行う
 */
SnapperService::~SnapperService()
{
}

/**
 * @brief SnapperServiceのシングルトンインスタンスを取得
 *
 * SnapperServiceのシングルトンインスタンスを返す
 * インスタンスが存在しない場合は新規作成する
 *
 * @return SnapperServiceのシングルトンインスタンス
 */
SnapperService* SnapperService::instance()
{
    if (!s_instance) {
        s_instance = new SnapperService();
    }
    return s_instance;
}

/**
 * @brief D-Busサービスへの再接続を試みる
 *
 * アイドルタイムアウトでヘルパープロセスが終了した場合など、D-Busインターフェースが無効になった時に呼び出す
 * QDBusInterfaceを再生成することでD-Bus activationが発動し、ヘルパープロセスが自動的に再起動される
 *
 * @return 再接続に成功した場合: true
 */
bool SnapperService::reconnect()
{
    qCInfo(snapperLog) << "D-Bus service lost, attempting to reconnect...";

    // startService() でヘルパーの起動完了を待ってから接続する
    // QDBusInterface 再生成だけでは activation の完了前に isValid() を確認してしまい、falseが返ることがある
    // Qt 6では startService() は QDBusReply<void> を返すため isValid() のみ確認する
    auto startReply = QDBusConnection::systemBus().interface()->startService("com.presire.qsnapper.Operations");
    if (!startReply.isValid()) {
        qCCritical(snapperLog) << "Failed to start D-Bus service:" << startReply.error().message();
        return false;
    }

    delete m_dbusInterface;
    m_dbusInterface = new QDBusInterface(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        QDBusConnection::systemBus(),
        this
    );

    if (!m_dbusInterface->isValid()) {
        qCCritical(snapperLog) << "Reconnection failed:" << QDBusConnection::systemBus().lastError().message();
        return false;
    }

    qCInfo(snapperLog) << "Reconnected to D-Bus service successfully.";
    return true;
}

/**
 * @brief D-Bus経由で利用可能なSnapper config一覧を取得
 * @return キャッシュされた設定名の一覧
 */
QStringList SnapperService::configs()
{
    // 注意:
    // ここで 遅延refreshConfigs()を呼ぶと、プロパティゲッター評価中にconfigsChangedシグナルが送信され、QML側でbinding loopとして検出される
    // 初回取得はQMLのComponent.onCompletedから明示的にrefreshConfigs()を呼ぶ契約とする
    return m_configs;
}

/**
 * @brief 設定一覧をD-Busから再取得する
 *
 * 取得した一覧をキャッシュし、現在の設定が一覧に無ければ先頭を採用する
 * 取得結果にかかわらずconfigsChanged()を発行する
 */
void SnapperService::refreshConfigs()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid; cannot list configs";
            return;
        }
    }

    QDBusReply<QStringList> reply = m_dbusInterface->call("ListConfigs");

    if (reply.isValid()) {
        m_configs = reply.value();
    }
    else {
        qCWarning(snapperLog) << "ListConfigs failed:" << reply.error().message();
        m_configs.clear();
    }

    m_configsChecked = true;
    emit configsChanged();

    // 現在のconfigが一覧に無ければ先頭を採用
    if (!m_configs.isEmpty() && !m_configs.contains(m_currentConfig)) {
        m_currentConfig = m_configs.first();
        emit currentConfigChanged();
    }
}

/**
 * @brief 接続できない場合に返すエラー応答のQDBusPendingCallを作る
 * @return 完了済みでエラーを保持するQDBusPendingCall
 * @note 呼び出し元がQDBusPendingCallWatcherで一律にエラーを扱えるよう、接続失敗も非同期呼び出しの失敗として返す
 */
static QDBusPendingCall connectionFailedCall()
{
    return QDBusPendingCall::fromError(
        QDBusError(QDBusError::NoServer,
                   QCoreApplication::translate("SnapperService", "D-Bus connection failed.")));
}

/**
 * @brief 認証を伴い得る設定系のメソッドを、長い待ち時間で非同期に呼び出す
 * @param method 呼び出すメソッド名
 * @param arguments メソッドの引数
 * @param timeoutMs 応答を待つ上限 (ミリ秒)
 * @return 応答を待つQDBusPendingCall、システムバスへ接続できない場合はエラー応答
 * @note 既定の待ち時間 (25秒) では、polkitの認証ダイアログへの入力やタイマ操作の完了を待つ間に応答が打ち切られ、
 *       実際には成功した操作が失敗と表示されるため、待ち時間を延ばす
 *       打ち切られた場合も呼び出し元は状態を読み直すため、表示が実際の状態と食い違ったままにはならない
 *       QDBusInterfaceの再生成 (reconnect) は同期呼び出しでGUIを止めるため使わず、固定の宛先へ直接送る
 *       特権サービスがアイドル終了していても、バスのD-Bus activationが起動するため再接続は不要である
 */
static QDBusPendingCall asyncSettingsCall(const QString &method, const QList<QVariant> &arguments,
                                          int timeoutMs = 5 * 60 * 1000)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return connectionFailedCall();
    }
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("com.presire.qsnapper.Operations"),
                                                          QStringLiteral("/com/presire/qsnapper/Operations"),
                                                          QStringLiteral("com.presire.qsnapper.Operations"),
                                                          method);
    message.setArguments(arguments);
    return bus.asyncCall(message, timeoutMs);
}

/**
 * @brief 指定設定のsnapper設定値を非同期に取得する (GetConfig)
 * @param configName 取得対象の設定名
 * @return 応答がa{ss}のQDBusPendingCall、接続できない場合はエラー応答
 * @note 同期呼び出しはGUIをブロックするため使用しない
 */
QDBusPendingCall SnapperService::asyncGetConfig(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("GetConfig"), {configName});
}

/**
 * @brief 指定設定のsnapper設定値を非同期に書き込む (WriteSnapperConfig)
 * @param configName 書き込み対象の設定名
 * @param settings 書き込む設定値
 * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
 * @note 値の妥当性はサーバ側が最終的に検証する (編集可能キー以外と不正値はInvalidArgsで拒否される)
 */
QDBusPendingCall SnapperService::asyncWriteSnapperConfig(const QString &configName,
                                                         const QMap<QString, QString> &settings)
{
    return asyncSettingsCall(QStringLiteral("WriteSnapperConfig"), {configName, QVariant::fromValue(settings)});
}

/**
 * @brief 指定設定のBtrfsクォータを非同期に設定する (SetupQuota)
 * @param configName 対象の設定名
 * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
 * @note 認可はサーバ側のpolkitで行われるため、認証ダイアログの表示中は応答が遅れる
 *       asyncGetConfigと同様に、長い待ち時間の非同期呼び出しを使い、同期のreconnectは行わない
 */
QDBusPendingCall SnapperService::asyncSetupQuota(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("SetupQuota"), {configName});
}

/**
 * @brief snapperのsystemdタイマの状態を非同期に取得する (GetTimerStates)
 * @return 応答がa{s(ss)}のQDBusPendingCall、接続できない場合はエラー応答
 */
QDBusPendingCall SnapperService::asyncGetTimerStates()
{
    return asyncSettingsCall(QStringLiteral("GetTimerStates"), {});
}

/**
 * @brief snapperのsystemdタイマの有効化と無効化を非同期に要求する (SetTimerEnabled)
 * @param unit 対象のユニット名
 * @param enabled trueで有効化して起動、falseで停止して無効化
 * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
 * @note 認可はサーバ側のpolkitで行われるため、認証ダイアログの表示中は応答が遅れる
 */
QDBusPendingCall SnapperService::asyncSetTimerEnabled(const QString &unit, bool enabled)
{
    return asyncSettingsCall(QStringLiteral("SetTimerEnabled"), {unit, enabled});
}

/**
 * @brief 設定の作成を非同期に要求する (CreateConfig)
 * @param configName 作成する設定名
 * @param subvolume 設定が対象とするBtrfsサブボリュームの絶対パス
 * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
 * @note polkitは毎回auth_adminで認証を求めるため、入力待ちとサブボリュームの作成時間を見込んで待ち時間を延ばす
 */
QDBusPendingCall SnapperService::asyncCreateConfig(const QString &configName, const QString &subvolume)
{
    return asyncSettingsCall(QStringLiteral("CreateConfig"), {configName, subvolume},
                             10 * 60 * 1000);
}

/**
 * @brief 設定の削除を非同期に要求する (DeleteConfig)
 * @param configName 削除する設定名
 * @return 応答がboolのQDBusPendingCall、接続できない場合はエラー応答
 * @note 対象設定の全スナップショットの削除を伴うため、待ち時間を長くする
 *       打ち切られた場合も呼び出し元は一覧を読み直すため、表示が実際の状態と食い違ったままにはならない
 */
QDBusPendingCall SnapperService::asyncDeleteConfig(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("DeleteConfig"), {configName},
                             40 * 60 * 1000);
}

/**
 * @brief 複数スナップショットの一括削除を非同期に要求する (DeleteSnapshots)
 * @param configName 対象の設定名
 * @param numbers 削除するスナップショット番号
 * @return 応答がauのQDBusPendingCall、接続できない場合はエラー応答
 * @note 応答は再読み込み後に削除を確認できた番号だけであり、要求より短い場合は一部失敗を意味する
 *       大量のスナップショットの削除を見込んで待ち時間を長くする
 */
QDBusPendingCall SnapperService::asyncDeleteSnapshots(const QString &configName, const QList<uint> &numbers)
{
    return asyncSettingsCall(QStringLiteral("DeleteSnapshots"),
                             {configName, QVariant::fromValue(numbers)},
                             40 * 60 * 1000);
}

/**
 * @brief 1件のスナップショットの削除を、ヘルパのDeleteSnapshotメソッドへ非同期に要求する
 * @param configName 削除するスナップショットが属するSnapperの設定名
 * @param number 削除するスナップショット番号 (呼び出し元のSnapshotListModelが1以上に限定する)
 * @return 応答の値がboolになるQDBusPendingCall
 *         ヘルパへ接続できない場合は、エラー応答を持つQDBusPendingCall
 * @note 同期呼び出しでは、認証ダイアログの表示中にGUIが固まり、D-Busの既定のタイムアウト (25秒) で打ち切られる
 *       そのため、非同期で呼び出す
 *       タイムアウト (10分) は、認証の入力にかかる時間と、qgroupの更新を伴う削除にかかる時間を見込んだ値である
 *       DeleteSnapshotの番号の引数はD-Busのint型のため、intへ変換して渡す
 */
QDBusPendingCall SnapperService::asyncDeleteSnapshot(const QString &configName, uint number)
{
    return asyncSettingsCall(QStringLiteral("DeleteSnapshot"),
                             {configName, static_cast<int>(number)},
                             10 * 60 * 1000);
}

/**
 * @brief 設定のファイルシステム使用量を非同期に取得する (GetFilesystemUsage)
 * @param configName 対象の設定名
 * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答
 */
QDBusPendingCall SnapperService::asyncGetFilesystemUsage(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("GetFilesystemUsage"), {configName});
}

/**
 * @brief 設定のサブボリューム一覧を非同期に取得する (ListSubvolumes)
 * @param configName 対象の設定名
 * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答
 * @note auth_admin_keepのため初回は認証ダイアログへの入力待ちが発生する
 */
QDBusPendingCall SnapperService::asyncListSubvolumes(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("ListSubvolumes"), {configName});
}

/**
 * @brief URLを外部ブラウザで開く
 *
 * デスクトップポータル経由で開く (失敗時はQDesktopServicesへ切り替える)
 * SELinuxが有効な場合、本アプリケーションはSELinuxのqsnapper_tで動作する
 * そのため、Webブラウザを直接起動するとqsnapper_tの制限下で動作する
 *
 * ポータル経由の場合は、Webブラウザはユーザセッション側のプロセスとして起動される
 *
 * ただし、httpとhttps以外のURLはアクセス不可とする
 *
 * @param url 開くURL
 * @return 開く処理を開始できた場合はtrue
 */
bool SnapperService::openExternalUrl(const QString &url)
{
    return qsnapper::desktop::openExternalUrl(QUrl(url, QUrl::StrictMode), this);
}

/**
 * @brief 現在の設定を変更する
 * @param name 新たな設定名
 */
void SnapperService::setCurrentConfig(const QString &name)
{
    if (name == m_currentConfig) return;
    m_currentConfig = name;
    emit currentConfigChanged();
}

/**
 * @brief スナップショット作成が許可されているか確認
 *
 * 環境変数DISABLE_SNAPSHOTSの設定に基づいて、指定されたタイプのスナップショット作成が許可されているか判定する
 *
 * @param snapshotType スナップショットのタイプ ("single", "around"など)
 * @return 作成が許可されている場合: true、それ以外: false
 */
bool SnapperService::createSnapshotAllowed(const QString &snapshotType) const
{
    QString disableSnapshots = qEnvironmentVariable("DISABLE_SNAPSHOTS");

    if (disableSnapshots.isEmpty()) {
        return true;
    }

    disableSnapshots = disableSnapshots.toLower().remove('-').remove('_').remove('.');
    QStringList disabledTypes = disableSnapshots.split(',', Qt::SkipEmptyParts);

    if (disabledTypes.contains("all")) {
        return false;
    }

    return !disabledTypes.contains(snapshotType.toLower());
}

/**
 * @brief シングルスナップショットを作成
 *
 * 単一のスナップショットを作成する
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合は-1を返す
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @return 成功時: 作成されたスナップショットの番号、失敗時: -1
 */
int SnapperService::createSingle(const QString &description,
                                 FsSnapshot::CleanupAlgorithm cleanup,
                                 bool important,
                                 const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("single")) {
        return -1;
    }

    return create(FsSnapshot::SnapshotType::Single, description, -1, cleanup, important, userdata);
}

/**
 * @brief Preスナップショットを作成
 *
 * 変更前のスナップショット (Pre)を作成する
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合は-1を返す
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @return 成功時: 作成されたスナップショットの番号、失敗時: -1
 */
int SnapperService::createPre(const QString &description,
                              FsSnapshot::CleanupAlgorithm cleanup,
                              bool important,
                              const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("around")) {
        return -1;
    }

    return create(FsSnapshot::SnapshotType::Pre, description, -1, cleanup, important, userdata);
}

/**
 * @brief Postスナップショットを作成
 *
 * 変更後のスナップショット (Post) を作成する
 * 対応するPreスナップショットとペアになる
 *
 * @param description スナップショットの説明
 * @param previousNumber 対応するPreスナップショットの番号
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @return 成功時: 作成されたスナップショットの番号、失敗時: -1
 */
int SnapperService::createPost(const QString &description,
                               int previousNumber,
                               FsSnapshot::CleanupAlgorithm cleanup,
                               bool important,
                               const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("around")) {
        return -1;
    }

    // 存在確認だけに使うため、find()が返したオブジェクトはこのスコープで解放する
    const std::unique_ptr<FsSnapshot> previous(find(previousNumber));

    if (!previous) {
        qCCritical(snapperLog) << "Previous filesystem snapshot was not found:" << previousNumber;
        emit snapshotCreationFailed(tr("Previous snapshot was not found."));
        return -1;
    }

    return create(FsSnapshot::SnapshotType::Post, description, previous->number(), cleanup, important, userdata);
}

/**
 * @brief 全てのスナップショットを取得
 *
 * D-Bus経由でSnapperに問い合わせ、全てのスナップショットのリストを取得する
 * 返したオブジェクトはparentを持たず、呼び出し側が所有する
 * QMLの所有権ヒューリスティックはリストの要素には適用されないため、C++から呼ぶこと
 *
 * @return スナップショットのリスト (呼び出し側が解放する)
 */
QList<FsSnapshot*> SnapperService::all()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            return QList<FsSnapshot*>();
        }
    }

    QDBusReply<QString> reply = m_dbusInterface->call("ListSnapshots", m_currentConfig);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to list snapshots via D-Bus:"
                               << reply.error().message();
        return QList<FsSnapshot*>();
    }

    QString csvOutput = reply.value();

    return parseSnapshotList(csvOutput);
}

/**
 * @brief 指定された番号のスナップショットを検索
 *
 * スナップショット番号を指定して、該当するスナップショットを検索する
 *
 * 一致しなかったオブジェクトはここで解放し、一致したものだけを呼び出し側へ渡す
 * (QMLから呼んだ場合はJavaScriptOwnershipとなりGCで解放される)
 *
 * @param number スナップショット番号
 * @return 見つかったスナップショット (呼び出し側が解放する)、見つからない場合はnullptr
 */
FsSnapshot* SnapperService::find(int number)
{
    FsSnapshot *found = nullptr;
    const QList<FsSnapshot*> snapshots = all();
    for (FsSnapshot *snapshot : snapshots) {
        if (!found && snapshot->number() == number) {
            found = snapshot;
        } else {
            delete snapshot;
        }
    }
    return found;
}

/**
 * @brief 指定されたスナップショットにロールバック
 *
 * D-Bus経由でスナップショットへのロールバックを実行する
 * 成功時はrollbackCompletedシグナル、失敗時はrollbackFailedシグナルを発行する
 *
 * @param number ロールバック先のスナップショット番号
 * @return ロールバックが成功した場合: true、それ以外: false
 */
bool SnapperService::rollback(int number)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            emit rollbackFailed(tr("D-Bus connection failed."));
            return false;
        }
    }

    QDBusReply<bool> reply = m_dbusInterface->call("RollbackSnapshot", m_currentConfig, number);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to rollback snapshot via D-Bus:"
                               << reply.error().message();
        emit rollbackFailed(tr("Failed to rollback snapshot: %1").arg(errorText(reply.error())));
        return false;
    }

    bool success = reply.value();
    if (success) {
        emit rollbackCompleted();
    }
    else {
        qCWarning(snapperLog) << "Rollback to snapshot" << number << "failed";
        emit rollbackFailed(tr("Rollback operation failed."));
    }

    return success;
}

/**
 * @brief 指定されたスナップショットを削除
 *
 * D-Bus経由でスナップショットの削除を実行する
 * 成功時はsnapshotDeletedシグナル、失敗時はsnapshotDeletionFailedシグナルを発行する
 *
 * @param number 削除するスナップショット番号
 * @return 削除が成功した場合: true、それ以外: false
 */
bool SnapperService::deleteSnapshot(int number)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            emit snapshotDeletionFailed(number, tr("D-Bus connection failed."));
            return false;
        }
    }

    QDBusReply<bool> reply = m_dbusInterface->call("DeleteSnapshot", m_currentConfig, number);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to delete snapshot via D-Bus:"
                              << reply.error().message();
        emit snapshotDeletionFailed(number, tr("Failed to delete snapshot: %1")
                                   .arg(errorText(reply.error())));
        return false;
    }

    bool success = reply.value();
    if (success) {
        emit snapshotDeleted(number);
    }
    else {
        qCWarning(snapperLog) << "Delete snapshot" << number << "failed";
        emit snapshotDeletionFailed(number, tr("Delete operation failed."));
    }

    return success;
}

/**
 * @brief スナップショットを作成 (内部実装)
 *
 * D-Bus経由でスナップショットを作成する
 * 作成成功時はsnapshotCreatedシグナル、失敗時はsnapshotCreationFailedシグナルを送信する
 *
 * @param snapshotType スナップショットのタイプ
 * @param description スナップショットの説明
 * @param previousNumber 対応するPreスナップショットの番号 (Postタイプ以外は-1)
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @return 成功時: 作成されたスナップショットの番号、失敗時: -1
 */
int SnapperService::create(FsSnapshot::SnapshotType snapshotType,
                           const QString &description,
                           int previousNumber,
                           FsSnapshot::CleanupAlgorithm cleanup,
                           bool important,
                           const QVariantMap &userdata)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            emit snapshotCreationFailed(tr("D-Bus connection failed."));
            return -1;
        }
    }

    QString type = FsSnapshot::snapshotTypeToString(snapshotType);
    // サーバ側のCreateSnapshotは空文字列 / number / timeline / empty-pre-postだけを受け付けるため、
    // クリーンアップなしは"none"ではなく空文字列で送る
    const QString cleanupStr = FsSnapshot::cleanupAlgorithmToString(cleanup);

    const QMap<QString, QString> userdataMap = toStringMap(userdata);
    QDBusReply<QString> reply = m_dbusInterface->call("CreateSnapshot",
                                                      m_currentConfig,
                                                      type,
                                                      description,
                                                      previousNumber,
                                                      cleanupStr,
                                                      QVariant::fromValue(userdataMap),
                                                      important);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to create snapshot via D-Bus:" << reply.error().message();
        emit snapshotCreationFailed(tr("Failed to create snapshot: %1").arg(errorText(reply.error())));
        return -1;
    }

    // 一覧の末尾ではなく、サーバーが応答で返した番号で作成されたスナップショットを特定する
    // (並行して作成された別のスナップショットとの取り違えを防ぐ)
    const int createdNumber = qsnapper::csv::parseCreatedSnapshotNumber(reply.value());
    // find()が返したオブジェクトはこのスコープで解放する (snapshotCreatedの受信側はシグナル発行中だけ参照できる)
    const std::unique_ptr<FsSnapshot> newSnapshot(createdNumber > 0 ? find(createdNumber) : nullptr);
    if (!newSnapshot) {
        qCWarning(snapperLog) << "Created snapshot could not be identified from the reply";
        emit snapshotCreationFailed(tr("The snapshot was created, but it could not be identified."));
        return -1;
    }

    emit snapshotCreated(newSnapshot.get());
    return createdNumber;
}

/**
 * @brief 既存スナップショットのメタデータを編集する
 *
 * description / cleanup / userdata を更新する
 * 成功時にsnapshotModifiedシグナル、失敗時にsnapshotModificationFailedシグナルを送信する
 *
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文
 * @param cleanup 新しいクリーンアップアルゴリズム文字列
 * @param userdata 新しいユーザデータマップ
 * @return 成功時: true、それ以外: false
 */
bool SnapperService::modifySnapshot(int number,
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            emit snapshotModificationFailed(number, tr("D-Bus connection failed."));
            return false;
        }
    }

    const QMap<QString, QString> userdataMap = toStringMap(userdata);
    QDBusReply<bool> reply = m_dbusInterface->call("ModifySnapshot",
                                                   m_currentConfig,
                                                   number,
                                                   description,
                                                   cleanup,
                                                   QVariant::fromValue(userdataMap));
    if (!reply.isValid()) {
        qCCritical(snapperLog) << "ModifySnapshot failed:" << reply.error().message();
        emit snapshotModificationFailed(number, tr("Failed to modify snapshot: %1").arg(errorText(reply.error())));
        return false;
    }

    const bool success = reply.value();
    if (success) {
        emit snapshotModified(number);
    }
    else {
        emit snapshotModificationFailed(number, tr("Modify operation failed."));
    }
    return success;
}

/**
 * @brief CSV形式のスナップショットリストをパース
 *
 * Snapperから取得したCSV形式の出力をパースし、FsSnapshotオブジェクトのリストに変換する
 *
 * 生成したオブジェクトはparentを持たず、所有権は呼び出し側に移る
 *
 * @param csvOutput CSV形式のスナップショットリスト
 * @return パースされたスナップショットのリスト (呼び出し側が解放する)
 */
QList<FsSnapshot*> SnapperService::parseSnapshotList(const QString &csvOutput)
{
    QList<FsSnapshot*> snapshots;

    QStringList lines = csvOutput.split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        return snapshots;
    }

    for (int i = 1; i < lines.size(); ++i) {
        QString line = lines[i];
        // 1行 = 1レコードの前提は、サーバー側がdescription等の制御文字 (改行含む) をU+FFFDに置き換えて出力するため維持される
        // 行分割は変更せず、フィールド分割のみRFC 4180対応のsplitRecord()へ置き換える
        // これによりカンマを含むdescription等が列ずれを起こさなくなる (無quoteの旧サーバー出力もそのまま解釈できる)
        QStringList fields = qsnapper::csv::splitRecord(line);

        // 引用符が閉じていない行はsplitRecord()が空のリストを返すため、ここで除外される
        if (fields.size() < 7) {
            continue;
        }

        bool ok;
        int number = fields[0].toInt(&ok);
        if (!ok || number == 0) {
            continue;
        }

        FsSnapshot::SnapshotType type = FsSnapshot::stringToSnapshotType(fields[1]);

        int previousNumber = fields[2].toInt(&ok);
        if (!ok) {
            previousNumber = -1;
        }

        QDateTime timestamp = QDateTime::fromString(fields[3], Qt::ISODate);
        if (!timestamp.isValid()) {
            timestamp = QDateTime();
            qCWarning(snapperLog) << "Error when parsing date/time:" << fields[3];
        }

        QString user = fields[4];

        FsSnapshot::CleanupAlgorithm cleanupAlgo = FsSnapshot::stringToCleanupAlgorithm(fields[5]);

        QString description = fields[6];

        // userdataをパースする (key1=value1,key2=value2形式)
        // 注意: 行はカンマで区切られているため、各userdataのペアは個別のフィールドとなっている
        QVariantMap userdata;
        for (int j = 7; j < fields.size(); ++j) {
            if (fields[j].isEmpty()) {
                continue;
            }

            int eqIdx = fields[j].indexOf('=');

            if (eqIdx > 0) {
                userdata[fields[j].left(eqIdx)] = fields[j].mid(eqIdx + 1);
            }
        }

        FsSnapshot *snapshot = new FsSnapshot(number, type, previousNumber, timestamp,
                                              user, cleanupAlgo, description, userdata);
        snapshots.append(snapshot);
    }

    return snapshots;
}
