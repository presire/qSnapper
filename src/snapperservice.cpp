/**
 * @file snapperservice.cpp
 * @brief スナップショット操作をqsnapper-dbus-serviceへ非同期に依頼するSnapperServiceの実装
 *
 * すべてのD-Bus呼び出しをQDBusPendingCallWatcherで受け取り、GUIを止めない
 * 結果はシグナルでQMLへ返し、処理中の操作数をoperationBusyとして公開する
 *
 * 利用する側 : SnapshotListModel、FileChangeModel、設定画面
 * 出力 : Q_PROPERTYの変更通知、各操作の完了シグナル
 * 依存 : Qt D-Bus、qsnapper::csv、snappertimerstate.h、externalurlopener.h
 */

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusMessage>
#include <QDBusError>
#include <QDBusMetaType>
#include <QMap>
#include <QDebug>
#include <memory>
#include "snapperservice.h"
#include "snappertimerstate.h"
#include "csvrecord.h"
#include "externalurlopener.h"

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
 * @param error qsnapper-dbus-serviceから返されたD-Busエラー
 * @return 既知の拒否理由であれば翻訳済みの固定文言、それ以外はエラー本文
 * @note qsnapper-dbus-serviceは復元や設定変更の実行中に操作を一律で拒否し、英語の固定文言だけを返す
 *       一時的な拒否は再実行で成功し得るが、認可の拒否は管理者認証が必要であるため、エラー名で判別して理由を翻訳して伝える
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
    // Polkitの認可が得られなかった場合 (認証ダイアログの取消、権限の無いユーザ、認証の失敗など)
    // バスの標準エラー名と、qsnapper-dbus-serviceが独自に返す名前の両方を同じ文言で扱う
    if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
        || name == QLatin1String("com.presire.qsnapper.Error.AccessDenied")
        || name == QLatin1String("com.presire.qsnapper.Error.NotAuthorized")) {
        return QCoreApplication::translate("SnapperService",
                                           "Authorization was denied. Administrator authentication is required for this operation.");
    }
    return error.message();
}

/**
 * @brief SnapperServiceのコンストラクタ
 *
 * D-Busでやり取りする型を登録し、現在の設定を既定値で初期化する
 *
 * @param parent 親オブジェクト
 * @note QDBusInterfaceを生成するとリモートオブジェクトの問い合わせでGUIが止まるため、接続は各呼び出し時に固定の宛先へ非同期で行う
 */
SnapperService::SnapperService(QObject *parent)
    : QObject(parent)
    , m_configsChecked(false)
    , m_configsGeneration(0)
    , m_pendingOperations(0)
    , m_currentConfig(QStringLiteral("root"))
{
    // QMap<QString, QString>のD-Bus型登録(クライアント側)
    qDBusRegisterMetaType<QMap<QString, QString>>();

    // GetTimerStatesの応答a{s(ss)}を復元するための型登録(構造体(ss)と、そのマップ)
    qDBusRegisterMetaType<SnapperTimerState>();
    qDBusRegisterMetaType<SnapperTimerStateMap>();

    // QDBusInterfaceはコンストラクタでリモートオブジェクトを同期的に問い合わせてGUIを止めるため、生成しない
    // すべての呼び出しは非同期の固定宛先で行い、特権サービスの起動はバスのD-Bus activationに任せる
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
 * @brief D-Bus経由で利用可能なSnapper config一覧を取得
 * @return キャッシュされた設定名の一覧
 */
QStringList SnapperService::configs()
{
    // getterからrefreshConfigs()を呼ぶと、プロパティを評価するたびに再取得が始まり、応答のconfigsChangedで再び評価されて取得が繰り返される
    // この循環を避けるため、getterはキャッシュを返すだけにして、初回の取得はQMLのComponent.onCompletedからrefreshConfigs()を明示的に呼んで行う
    return m_configs;
}

// asyncSettingsCall()の定義は、非同期APIをまとめた位置にあるため、ここでは宣言だけを置く
// 待ち時間を省略した呼び出しは5分で打ち切られる
static QDBusPendingCall asyncSettingsCall(const QString &method, const QList<QVariant> &arguments,
                                          int timeoutMs = 5 * 60 * 1000);

/**
 * @brief 設定一覧をD-Busから非同期に再取得する
 *
 * 取得した一覧をキャッシュし、現在の設定が一覧に無ければ先頭を採用する
 * 取得結果にかかわらず、応答の受信後にconfigsChanged()を発行する
 *
 * @note 同期呼び出しでは、ヘルパの起動待ちや応答の遅延の間にGUIが固まるため、QDBusPendingCallWatcherで応答を受け取る
 *       連続して呼ばれた場合は、世代番号 (m_configsGeneration) が一致する最新の応答だけを反映し、古い応答は捨てる
 *       ヘルパの起動はバスのD-Bus activationに任せる
 */
void SnapperService::refreshConfigs()
{
    const quint64 generation = ++m_configsGeneration;
    auto *watcher = new QDBusPendingCallWatcher(
        asyncSettingsCall(QStringLiteral("ListConfigs"), {}, 60 * 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_configsGeneration) {
            return;
        }

        if (watcher->isError()) {
            qCWarning(snapperLog) << "ListConfigs failed:" << watcher->error().message();
            m_configs.clear();
        }
        else {
            const QDBusPendingReply<QStringList> reply = *watcher;
            m_configs = reply.value();
        }

        m_configsChecked = true;
        emit configsChanged();

        // 現在のconfigが一覧に無ければ先頭を採用
        if (!m_configs.isEmpty() && !m_configs.contains(m_currentConfig)) {
            m_currentConfig = m_configs.first();
            emit currentConfigChanged();
        }
    });
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
 * @note D-Busの既定の待ち時間 (25秒) のままでは、Polkitの認証ダイアログへの入力やタイマ操作の完了を待つ間に応答が打ち切られ、
 *       実際には成功した操作が失敗と表示されるため、待ち時間を延ばす
 *       打ち切られた場合も呼び出し元は状態を読み直すため、表示が実際の状態と食い違ったままにはならない
 *       QDBusInterfaceの生成は同期呼び出しでGUIを止めるため使わず、固定の宛先へ直接送る
 *       特権サービスがアイドル終了していても、バスのD-Bus activationが起動するため再接続は不要である
 */
static QDBusPendingCall asyncSettingsCall(const QString &method, const QList<QVariant> &arguments,
                                          int timeoutMs)
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
 * @note 認可はサーバ側のPolkitで行われるため、認証ダイアログの表示中は応答が遅れる
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
 * @note 認可はサーバ側のPolkitで行われるため、認証ダイアログの表示中は応答が遅れる
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
 * @note Polkitは毎回auth_adminで認証を求めるため、入力待ちとサブボリュームの作成時間を見込んで待ち時間を延ばす
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
 * @brief 指定設定のスナップショット一覧を非同期に取得する (ListSnapshots)
 * @param configName 対象の設定名
 * @return 応答がCSV文字列のQDBusPendingCall、接続できない場合はエラー応答
 * @note 同期呼び出しでは、snapperdの応答待ちや認証ダイアログの表示中にGUIが固まるため、非同期で呼び出す
 *       認証の入力待ちを見込み、待ち時間は10分とする
 */
QDBusPendingCall SnapperService::asyncListSnapshots(const QString &configName)
{
    return asyncSettingsCall(QStringLiteral("ListSnapshots"), {configName}, 10 * 60 * 1000);
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
 * @param snapshotType スナップショットの種別 ("single", "around"など)
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
 * @brief スナップショット作成の受付可否を確認し、不可なら失敗を通知する
 * @param snapshotType 作成するスナップショットの種別 (DISABLE_SNAPSHOTSの判定に使う文字列)
 * @return 作成を続けてよい場合はtrue、環境変数により無効化されている場合はfalse
 * @note 無効化されている場合はsnapshotCreationFailedシグナルを発行する
 *       呼び出し元に何も表示されないままボタン操作が無視されるのを避けるため、理由を通知する
 */
bool SnapperService::checkCreateAllowed(const QString &snapshotType)
{
    if (createSnapshotAllowed(snapshotType)) {
        return true;
    }

    emit snapshotCreationFailed(tr("Creating this type of snapshot is disabled by the DISABLE_SNAPSHOTS environment variable."));
    return false;
}

/**
 * @brief シングルスナップショットを非同期に作成
 *
 * 単一のスナップショットを作成する
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合は、snapshotCreationFailedシグナルを発行する
 * 作成の結果はsnapshotCreated / snapshotCreationFailedシグナルで通知する
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 */
void SnapperService::createSingle(const QString &description,
                                  FsSnapshot::CleanupAlgorithm cleanup,
                                  bool important,
                                  const QVariantMap &userdata)
{
    if (!checkCreateAllowed(QStringLiteral("single"))) {
        return;
    }

    create(FsSnapshot::SnapshotType::Single, description, -1, cleanup, important, userdata, m_currentConfig);
}

/**
 * @brief Preスナップショットを非同期に作成
 *
 * 変更前のスナップショット(Pre)を作成する
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合は、snapshotCreationFailedシグナルを発行する
 * 作成の結果はsnapshotCreated / snapshotCreationFailedシグナルで通知する
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 */
void SnapperService::createPre(const QString &description,
                               FsSnapshot::CleanupAlgorithm cleanup,
                               bool important,
                               const QVariantMap &userdata)
{
    if (!checkCreateAllowed(QStringLiteral("around"))) {
        return;
    }

    create(FsSnapshot::SnapshotType::Pre, description, -1, cleanup, important, userdata, m_currentConfig);
}

/**
 * @brief Postスナップショットを非同期に作成
 *
 * 変更後のスナップショット(Post)を作成する
 * 対応するPreスナップショットとペアになる
 * 作成の結果はsnapshotCreated / snapshotCreationFailedシグナルで通知する
 *
 * @param description スナップショットの説明
 * @param previousNumber 対応するPreスナップショットの番号
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @note 紐付け先のPreが存在することを、一覧を非同期に取得して確認してから作成を要求する
 *       確認から作成までの間に設定が切り替わっても、確認した設定に対して作成するよう設定名を先に控える
 */
void SnapperService::createPost(const QString &description,
                                int previousNumber,
                                FsSnapshot::CleanupAlgorithm cleanup,
                                bool important,
                                const QVariantMap &userdata)
{
    if (!checkCreateAllowed(QStringLiteral("around"))) {
        return;
    }

    const QString configName = m_currentConfig;
    beginOperation();
    auto *watcher = new QDBusPendingCallWatcher(asyncListSnapshots(configName), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, description, previousNumber, cleanup, important, userdata, configName]() {
        watcher->deleteLater();

        if (watcher->isError()) {
            qCCritical(snapperLog) << "Failed to list snapshots via D-Bus:" << watcher->error().message();
            emit snapshotCreationFailed(tr("Failed to create snapshot: %1").arg(errorText(watcher->error())));
            endOperation();
            return;
        }

        const QDBusPendingReply<QString> reply = *watcher;
        const QList<FsSnapshot*> snapshots = parseSnapshotList(reply.value());
        bool previousExists = false;
        for (const FsSnapshot *snapshot : snapshots) {
            if (snapshot->number() == previousNumber) {
                previousExists = true;
                break;
            }
        }
        qDeleteAll(snapshots);

        if (!previousExists) {
            qCCritical(snapperLog) << "Previous filesystem snapshot was not found:" << previousNumber;
            emit snapshotCreationFailed(tr("Previous snapshot was not found."));
            endOperation();
            return;
        }

        // create()が自身で処理中の状態を開始するため、その後でこの確認分の処理中の状態を終える
        // 先に終えると、処理中の状態が一瞬だけ解除されて画面の表示が点滅する
        create(FsSnapshot::SnapshotType::Post, description, previousNumber, cleanup, important, userdata, configName);
        endOperation();
    });
}

/**
 * @brief 指定されたスナップショットへ非同期にロールバック
 *
 * D-Bus経由でスナップショットへのロールバックを要求する
 * 成功時はrollbackCompletedシグナル、失敗時はrollbackFailedシグナルを発行する
 *
 * @param number ロールバック先のスナップショット番号
 * @note ロールバックは、認証の入力待ちとサブボリュームの切り替えに時間がかかるため、待ち時間を40分とする
 *       処理中はoperationBusyがtrueになり、完了または失敗のシグナルを発行する直前にfalseへ戻る
 */
void SnapperService::rollback(int number)
{
    beginOperation();
    const QString configName = m_currentConfig;
    auto *watcher = new QDBusPendingCallWatcher(
        asyncSettingsCall(QStringLiteral("RollbackSnapshot"), {configName, number}, 40 * 60 * 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, number]() {
        watcher->deleteLater();
        endOperation();

        if (watcher->isError()) {
            qCCritical(snapperLog) << "Failed to rollback snapshot via D-Bus:" << watcher->error().message();
            emit rollbackFailed(tr("Failed to rollback snapshot: %1").arg(errorText(watcher->error())));
            return;
        }

        const QDBusPendingReply<bool> reply = *watcher;
        if (reply.value()) {
            emit rollbackCompleted();
        }
        else {
            qCWarning(snapperLog) << "Rollback to snapshot" << number << "failed";
            emit rollbackFailed(tr("Rollback operation failed."));
        }
    });
}

/**
 * @brief 指定されたスナップショットを非同期に削除
 *
 * D-Bus経由でスナップショットの削除を要求する
 * 成功時はsnapshotDeletedシグナル、失敗時はsnapshotDeletionFailedシグナルを発行する
 *
 * @param number 削除するスナップショット番号
 * @note 削除の要求にはasyncDeleteSnapshot()を使い、認証の入力待ちを見込んだ待ち時間で呼び出す
 */
void SnapperService::deleteSnapshot(int number)
{
    beginOperation();
    auto *watcher = new QDBusPendingCallWatcher(
        asyncDeleteSnapshot(m_currentConfig, static_cast<uint>(qMax(number, 0))), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, number]() {
        watcher->deleteLater();
        endOperation();

        if (watcher->isError()) {
            qCCritical(snapperLog) << "Failed to delete snapshot via D-Bus:" << watcher->error().message();
            emit snapshotDeletionFailed(number, tr("Failed to delete snapshot: %1").arg(errorText(watcher->error())));
            return;
        }

        const QDBusPendingReply<bool> reply = *watcher;
        if (reply.value()) {
            emit snapshotDeleted(number);
        }
        else {
            qCWarning(snapperLog) << "Delete snapshot" << number << "failed";
            emit snapshotDeletionFailed(number, tr("Delete operation failed."));
        }
    });
}

/**
 * @brief スナップショットを非同期に作成 (内部実装)
 *
 * D-Bus経由でスナップショットの作成を要求する
 * 作成成功時はsnapshotCreatedシグナル、失敗時はsnapshotCreationFailedシグナルを発行する
 *
 * @param snapshotType スナップショットの種別
 * @param description スナップショットの説明
 * @param previousNumber 対応するPreスナップショットの番号 (Postタイプ以外は-1)
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 * @param configName 作成先の設定名 (呼び出し時点の設定を渡し、完了までの間に設定が切り替わっても取り違えない)
 * @note 作成後は、サーバが応答で返した番号を一覧から探して作成済みのスナップショットを特定する
 *       一覧の取得も非同期に行い、特定できるまで処理中の状態 (operationBusy) を維持する
 */
void SnapperService::create(FsSnapshot::SnapshotType snapshotType,
                            const QString &description,
                            int previousNumber,
                            FsSnapshot::CleanupAlgorithm cleanup,
                            bool important,
                            const QVariantMap &userdata,
                            const QString &configName)
{
    const QString type = FsSnapshot::snapshotTypeToString(snapshotType);
    // qsnapper-dbus-serviceのCreateSnapshotは空文字列、number、timeline、empty-pre-postだけを受け付けるため、
    // クリーンアップなしは"none"ではなく空文字列で送る
    const QString cleanupStr = FsSnapshot::cleanupAlgorithmToString(cleanup);

    const QMap<QString, QString> userdataMap = toStringMap(userdata);
    beginOperation();
    auto *watcher = new QDBusPendingCallWatcher(
        asyncSettingsCall(QStringLiteral("CreateSnapshot"),
                          {configName, type, description, previousNumber, cleanupStr,
                           QVariant::fromValue(userdataMap), important},
                          10 * 60 * 1000),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, configName]() {
        watcher->deleteLater();

        if (watcher->isError()) {
            qCCritical(snapperLog) << "Failed to create snapshot via D-Bus:" << watcher->error().message();
            emit snapshotCreationFailed(tr("Failed to create snapshot: %1").arg(errorText(watcher->error())));
            endOperation();
            return;
        }

        // 一覧の末尾ではなく、サーバが応答で返した番号で作成されたスナップショットを特定する
        // (並行して作成された別のスナップショットとの取り違えを防ぐ)
        const QDBusPendingReply<QString> reply = *watcher;
        const int createdNumber = qsnapper::csv::parseCreatedSnapshotNumber(reply.value());
        if (createdNumber <= 0) {
            qCWarning(snapperLog) << "Created snapshot could not be identified from the reply";
            emit snapshotCreationFailed(tr("The snapshot was created, but it could not be identified."));
            endOperation();
            return;
        }

        auto *listWatcher = new QDBusPendingCallWatcher(asyncListSnapshots(configName), this);
        connect(listWatcher, &QDBusPendingCallWatcher::finished, this, [this, listWatcher, createdNumber]() {
            listWatcher->deleteLater();

            std::unique_ptr<FsSnapshot> newSnapshot;
            if (!listWatcher->isError()) {
                const QDBusPendingReply<QString> listReply = *listWatcher;
                const QList<FsSnapshot*> snapshots = parseSnapshotList(listReply.value());
                for (FsSnapshot *snapshot : snapshots) {
                    if (!newSnapshot && snapshot->number() == createdNumber) {
                        newSnapshot.reset(snapshot);
                    }
                    else {
                        delete snapshot;
                    }
                }
            }
            endOperation();

            if (!newSnapshot) {
                qCWarning(snapperLog) << "Created snapshot could not be found in the snapshot list";
                emit snapshotCreationFailed(tr("The snapshot was created, but it could not be identified."));
                return;
            }

            // snapshotCreatedの受信側は、シグナル発行中だけ参照できる
            emit snapshotCreated(newSnapshot.get());
        });
    });
}

/**
 * @brief 既存スナップショットのメタデータを非同期に編集する
 *
 * description / cleanup / userdata を更新する
 * 成功時にsnapshotModifiedシグナル、失敗時にsnapshotModificationFailedシグナルを送信する
 *
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文
 * @param cleanup 新しいクリーンアップアルゴリズム文字列
 * @param userdata 新しいユーザデータマップ
 * @note 認証の入力待ちを見込み、待ち時間を10分とする
 *       処理中はoperationBusyがtrueになり、完了または失敗のシグナルを発行する直前にfalseへ戻る
 */
void SnapperService::modifySnapshot(int number,
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata)
{
    const QMap<QString, QString> userdataMap = toStringMap(userdata);
    beginOperation();
    auto *watcher = new QDBusPendingCallWatcher(
        asyncSettingsCall(QStringLiteral("ModifySnapshot"),
                          {m_currentConfig, number, description, cleanup, QVariant::fromValue(userdataMap)},
                          10 * 60 * 1000),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, number]() {
        watcher->deleteLater();
        endOperation();

        if (watcher->isError()) {
            qCCritical(snapperLog) << "ModifySnapshot failed:" << watcher->error().message();
            emit snapshotModificationFailed(number, tr("Failed to modify snapshot: %1").arg(errorText(watcher->error())));
            return;
        }

        const QDBusPendingReply<bool> reply = *watcher;
        if (reply.value()) {
            emit snapshotModified(number);
        }
        else {
            emit snapshotModificationFailed(number, tr("Modify operation failed."));
        }
    });
}

/**
 * @brief 処理中の操作を1件増やし、0件から1件になった場合にoperationBusyChangedを発行する
 */
void SnapperService::beginOperation()
{
    ++m_pendingOperations;
    if (m_pendingOperations == 1) {
        emit operationBusyChanged();
    }
}

/**
 * @brief 処理中の操作を1件減らし、0件になった場合にoperationBusyChangedを発行する
 * @note 対応するbeginOperation()より多く呼ばれても0未満にならないようにする
 */
void SnapperService::endOperation()
{
    if (m_pendingOperations <= 0) {
        return;
    }

    --m_pendingOperations;
    if (m_pendingOperations == 0) {
        emit operationBusyChanged();
    }
}

/**
 * @brief CSV形式のスナップショットリストを解析する
 *
 * qsnapper-dbus-serviceがlibsnapperの情報から生成したCSV形式の出力を解析し、FsSnapshotオブジェクトのリストに変換する
 *
 * 生成したオブジェクトはparentを持たず、所有権は呼び出し側に移る
 *
 * @param csvOutput CSV形式のスナップショットリスト
 * @return 解析したスナップショットのリスト (呼び出し側が解放する)
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
        // 1行が1レコードである前提は、qsnapper-dbus-serviceがdescription等の制御文字(改行含む)をU+FFFDに置き換えて出力するため維持される
        // フィールドだけをRFC 4180に対応したsplitRecord()で分割し、行の分割は変えない
        // カンマを含むdescription等が列ずれを起こさなくなる(引用符のない旧サーバ出力もそのまま解釈できる)
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

        // userdataはフィールド7以降にkey=valueの組で並ぶため、各組をキーと値へ分解する
        // 行はカンマで区切られているため、各組は個別のフィールドとして届く
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
