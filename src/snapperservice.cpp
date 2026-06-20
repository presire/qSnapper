#include <QProcess>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QRegularExpression>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusMetaType>
#include <QMap>
#include <QDebug>
#include "snapperservice.h"

// QVariantMap --> QMap<QString,QString> 変換ヘルパー
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
 * @brief SnapperServiceのコンストラクタ
 *
 * D-Busインターフェースを初期化し、Snapperサービスへの接続を確立する
 *
 * @param parent 親オブジェクト
 */
SnapperService::SnapperService(QObject *parent)
    : QObject(parent)
    , m_configured(false)
    , m_configuredChecked(false)
    , m_configureOnInstall(false)
    , m_dbusInterface(nullptr)
    , m_configsChecked(false)
    , m_currentConfig(QStringLiteral("root"))
{
    // QMap<QString,QString> の D-Bus 型登録 (クライアント側)
    qDBusRegisterMetaType<QMap<QString, QString>>();

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
 * @brief Snapperが設定されているか確認
 *
 * Snapperが正しく設定されているかを確認する
 * 結果はキャッシュされ、2回目以降の呼び出しではキャッシュ値を返す
 *
 * @return Snapperが設定されている場合: true、それ以外: false
 */
bool SnapperService::isConfigured()
{
    if (m_configuredChecked) {
        return m_configured;
    }

    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid; cannot check configured";
            m_configured = false;
            m_configuredChecked = true;
            return false;
        }
    }

    QDBusReply<bool> reply = m_dbusInterface->call("IsConfigured");
    m_configured = reply.isValid() && reply.value();
    m_configuredChecked = true;

    return m_configured;
}

/**
 * @brief D-Bus経由で利用可能なSnapper config一覧を取得
 */
QStringList SnapperService::configs()
{
    // 注意:
    // ここで 遅延refreshConfigs() を呼ぶと、プロパティゲッター評価中にconfigsChangedシグナルが送信され、QML側でbinding loopとして検出される
    // 初回取得はQMLのComponent.onCompletedから明示的に refreshConfigs() を呼ぶ契約とする
    return m_configs;
}

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

void SnapperService::setCurrentConfig(const QString &name)
{
    if (name == m_currentConfig) return;
    m_currentConfig = name;
    emit currentConfigChanged();
}

/**
 * @brief Snapperを設定
 *
 * Snapperの初期設定を実行する
 * 設定完了後、configuredChangedシグナルを発行する
 */
void SnapperService::configureSnapper()
{
    m_configuredChecked = false;

    installationHelperStep4();
    writeSnapperConfig();
    updateEtcSysconfigYast2();
    setupSnapperQuota();

    emit configuredChanged(isConfigured());
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
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合はnullptrを返す
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 成功時: 作成されたスナップショット、失敗時: nullptr
 */
FsSnapshot* SnapperService::createSingle(const QString &description,
                                         FsSnapshot::CleanupAlgorithm cleanup,
                                         bool important,
                                         const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("single")) {
        return nullptr;
    }

    return create(FsSnapshot::SnapshotType::Single, description, nullptr, cleanup, important, userdata);
}

/**
 * @brief Preスナップショットを作成
 *
 * 変更前のスナップショット (Pre)を作成する
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合はnullptrを返す
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 成功時: 作成されたスナップショット、失敗時: nullptr
 */
FsSnapshot* SnapperService::createPre(const QString &description,
                                      FsSnapshot::CleanupAlgorithm cleanup,
                                      bool important,
                                      const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("around")) {
        return nullptr;
    }

    return create(FsSnapshot::SnapshotType::Pre, description, nullptr, cleanup, important, userdata);
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
 * @return 成功時: 作成されたスナップショット、失敗時: nullptr
 */
FsSnapshot* SnapperService::createPost(const QString &description,
                                       int previousNumber,
                                       FsSnapshot::CleanupAlgorithm cleanup,
                                       bool important,
                                       const QVariantMap &userdata)
{
    if (!createSnapshotAllowed("around")) {
        return nullptr;
    }

    FsSnapshot *previous = find(previousNumber);

    if (!previous) {
        qCCritical(snapperLog) << "Previous filesystem snapshot was not found:" << previousNumber;
        emit snapshotCreationFailed(tr("Previous snapshot was not found."));
        return nullptr;
    }

    return create(FsSnapshot::SnapshotType::Post, description, previous, cleanup, important, userdata);
}

/**
 * @brief 全てのスナップショットを取得
 *
 * D-Bus経由でSnapperに問い合わせ、全てのスナップショットのリストを取得する
 *
 * @return スナップショットのリスト
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
 * @param number スナップショット番号
 * @return 見つかったスナップショット、見つからない場合はnullptr
 */
FsSnapshot* SnapperService::find(int number)
{
    QList<FsSnapshot*> snapshots = all();
    for (FsSnapshot *snapshot : snapshots) {
        if (snapshot->number() == number) {
            return snapshot;
        }
    }
    return nullptr;
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
        emit rollbackFailed(tr("Failed to rollback snapshot: %1")
                           .arg(reply.error().message()));
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
                                   .arg(reply.error().message()));
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
 * @param previous 前のスナップショット (Postタイプの場合)
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 成功時: 作成されたスナップショット、失敗時: nullptr
 */
FsSnapshot* SnapperService::create(FsSnapshot::SnapshotType snapshotType,
                                   const QString &description,
                                   FsSnapshot *previous,
                                   FsSnapshot::CleanupAlgorithm cleanup,
                                   bool important,
                                   const QVariantMap &userdata)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid";
            emit snapshotCreationFailed(tr("D-Bus connection failed."));
            return nullptr;
        }
    }

    QString type = FsSnapshot::snapshotTypeToString(snapshotType);
    int preNumber = previous ? previous->number() : -1;
    QString cleanupStr = FsSnapshot::cleanupAlgorithmToString(cleanup);
    if (cleanupStr.isEmpty()) {
        cleanupStr = "none";
    }

    const QMap<QString, QString> userdataMap = toStringMap(userdata);
    QDBusReply<QString> reply = m_dbusInterface->call("CreateSnapshot",
                                                      m_currentConfig,
                                                      type,
                                                      description,
                                                      preNumber,
                                                      cleanupStr,
                                                      QVariant::fromValue(userdataMap),
                                                      important);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to create snapshot via D-Bus:" << reply.error().message();
        emit snapshotCreationFailed(tr("Failed to create snapshot: %1").arg(reply.error().message()));
        return nullptr;
    }

    // スナップショットリストを再取得して最新のスナップショットを返す
    QList<FsSnapshot*> snapshots = all();
    if (!snapshots.isEmpty()) {
        FsSnapshot *newSnapshot = snapshots.last();
        emit snapshotCreated(newSnapshot);
        return newSnapshot;
    }

    return nullptr;
}

/**
 * @brief 既存スナップショットのメタデータを編集
 *
 * description / cleanup / userdata を更新する
 * 成功時にsnapshotModifiedシグナル、失敗時にsnapshotModificationFailedシグナルを送信する
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
        emit snapshotModificationFailed(number, tr("Failed to modify snapshot: %1").arg(reply.error().message()));
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
 * @brief ターゲットルートパスを取得
 *
 * インストール中かどうかを判定し、適切なルートパスを返す
 *
 * @return ルートパス (通常は"/"、インストール中は"/mnt"など)
 */
QString SnapperService::targetRoot() const
{
    if (!nonSwitchedInstallation()) {
        return QStringLiteral("/");
    }

    return qEnvironmentVariable("YAST_INSTALLATION_DESTDIR", "/mnt");
}

/**
 * @brief 非スイッチインストール環境かどうかを判定
 *
 * 現在のシステムが非スイッチインストール環境かどうかを返す
 *
 * @return 常にfalse (現在の実装では未使用)
 */
bool SnapperService::nonSwitchedInstallation() const
{
    return false;
}

/**
 * @brief インストールヘルパーステップ4を実行
 *
 * Snapperのインストールヘルパースクリプトのステップ4を実行しす
 */
void SnapperService::installationHelperStep4()
{
    QStringList args;
    args << "--step" << "4";

    bool success = false;
    executeCommand("/usr/lib/snapper/installation-helper", args, success);
}

/**
 * @brief Snapper設定を書き込む
 *
 * Snapperの各種設定パラメータを設定する
 * NUMBER_CLEANUP、NUMBER_LIMIT、TIMELINE_CREATEなどの設定を行う
 */
void SnapperService::writeSnapperConfig()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid; cannot write config";
            return;
        }
    }

    QMap<QString, QString> settings;
    settings["NUMBER_CLEANUP"] = "yes";
    settings["NUMBER_LIMIT"] = "2-10";
    settings["NUMBER_LIMIT_IMPORTANT"] = "4-10";
    settings["TIMELINE_CREATE"] = "no";

    QDBusReply<bool> reply = m_dbusInterface->call("WriteSnapperConfig",
                                                    m_currentConfig,
                                                    QVariant::fromValue(settings));
    if (!reply.isValid() || !reply.value()) {
        qCWarning(snapperLog) << "WriteSnapperConfig failed:" << reply.error().message();
    }
}

/**
 * @brief /etc/sysconfig/yast2を更新
 *
 * /etc/sysconfig/yast2ファイル内のUSE_SNAPPER設定を"yes"に更新する
 * 設定が存在しない場合は新規追加する
 */
void SnapperService::updateEtcSysconfigYast2()
{
    QString sysconfigPath = QStringLiteral("/etc/sysconfig/yast2");
    QFile file(sysconfigPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(snapperLog) << "Could not open" << sysconfigPath << "for reading";
        return;
    }

    QTextStream in(&file);
    QString content = in.readAll();
    file.close();

    QRegularExpression regex("^USE_SNAPPER=.*$", QRegularExpression::MultilineOption);
    if (content.contains(regex)) {
        content.replace(regex, "USE_SNAPPER=\"yes\"");
    }
    else {
        content.append("\nUSE_SNAPPER=\"yes\"\n");
    }

    QSaveFile saveFile(sysconfigPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(snapperLog) << "Could not open" << sysconfigPath << "for writing";
        return;
    }

    QTextStream out(&saveFile);
    out << content;
    out.flush();

    if (!saveFile.commit()) {
        qCWarning(snapperLog) << "Could not atomically update" << sysconfigPath;
    }
}

/**
 * @brief Snapperのクォータを設定
 *
 * Snapperのクォータ機能を設定する
 */
void SnapperService::setupSnapperQuota()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnect()) {
            qCCritical(snapperLog) << "D-Bus interface is not valid; cannot setup quota";
            return;
        }
    }

    QDBusReply<bool> reply = m_dbusInterface->call("SetupQuota", m_currentConfig);
    if (!reply.isValid() || !reply.value()) {
        qCWarning(snapperLog) << "SetupQuota failed:" << reply.error().message();
    }
}

/**
 * @brief CSV形式のスナップショットリストをパース
 *
 * Snapperから取得したCSV形式の出力をパースし、FsSnapshotオブジェクトのリストに変換する
 *
 * @param csvOutput CSV形式のスナップショットリスト
 * @return パースされたスナップショットのリスト
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
        QStringList fields = line.split(',');

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

        // Parse userdata (key1=value1,key2=value2 format)
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
                                              user, cleanupAlgo, description, userdata, this);
        snapshots.append(snapshot);
    }

    return snapshots;
}

/**
 * @brief コマンドを実行
 *
 * 指定されたプログラムを引数付きで実行し、出力を取得する
 * xx秒のタイムアウトが設定されている
 *
 * @param program 実行するプログラムのパス
 * @param arguments プログラムに渡す引数のリスト
 * @param success 実行成功フラグ (出力パラメータ)
 * @return プログラムの標準出力
 */
QString SnapperService::executeCommand(const QString &program,
                                       const QStringList &arguments,
                                       bool &success)
{
    QProcess process;
    process.start(program, arguments);

    if (!process.waitForStarted()) {
        qCWarning(snapperLog) << "Failed to start process:" << program;
        success = false;
        return QString();
    }

    if (!process.waitForFinished(0)) {
        qCWarning(snapperLog) << "Process timeout:" << program;
        process.kill();
        success = false;
        return QString();
    }

    success = (process.exitCode() == 0);
    QString output = QString::fromUtf8(process.readAllStandardOutput());

    if (!success) {
        QString errorOutput = QString::fromUtf8(process.readAllStandardError());
        qCWarning(snapperLog) << "Command failed:" << program << arguments
                              << "Error:" << errorOutput;
    }

    return output;
}
