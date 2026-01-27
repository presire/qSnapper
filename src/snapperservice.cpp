#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QDebug>
#include <QRegularExpression>
#include <QDBusConnection>
#include <QDBusReply>
#include <QDBusError>
#include "snapperservice.h"

Q_LOGGING_CATEGORY(snapperLog, "qsnapper")

SnapperService* SnapperService::s_instance = nullptr;

/**
 * @brief SnapperServiceのコンストラクタ
 *
 * D-Busインターフェースを初期化し、Snapperサービスへの接続を確立します。
 *
 * @param parent 親オブジェクト
 */
SnapperService::SnapperService(QObject *parent)
    : QObject(parent)
    , m_configured(false)
    , m_configuredChecked(false)
    , m_configureOnInstall(false)
    , m_dbusInterface(nullptr)
{
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
 * リソースのクリーンアップを行います。
 */
SnapperService::~SnapperService()
{
}

/**
 * @brief SnapperServiceのシングルトンインスタンスを取得
 *
 * SnapperServiceのシングルトンインスタンスを返します。
 * インスタンスが存在しない場合は新規作成します。
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
 * @brief Snapperが設定されているか確認
 *
 * Snapperが正しく設定されているかを確認します。
 * 結果はキャッシュされ、2回目以降の呼び出しではキャッシュ値を返します。
 *
 * @return Snapperが設定されている場合はtrue、それ以外はfalse
 */
bool SnapperService::isConfigured()
{
    if (m_configuredChecked) {
        return m_configured;
    }

    QString root = targetRoot();
    QStringList args;
    args << "--no-dbus" << "--root" << root << "--csvout" << "list-configs"
         << "--columns" << "config,subvolume";

    bool success = false;
    QString output = executeCommand("/usr/bin/snapper", args, success);

    m_configured = success && output.contains("root,");
    m_configuredChecked = true;

    return m_configured;
}

/**
 * @brief Snapperを設定
 *
 * Snapperの初期設定を実行します。
 * 設定完了後、configuredChangedシグナルを発行します。
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
 * 環境変数DISABLE_SNAPSHOTSの設定に基づいて、
 * 指定されたタイプのスナップショット作成が許可されているか判定します。
 *
 * @param snapshotType スナップショットのタイプ ("single", "around"など)
 * @return 作成が許可されている場合はtrue、それ以外はfalse
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
 * 単一のスナップショットを作成します。
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合はnullptrを返します。
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 作成されたスナップショット、失敗時はnullptr
 */
FsSnapshot* SnapperService::createSingle(const QString &description,
                                         FsSnapshot::CleanupAlgorithm cleanup,
                                         bool important)
{
    if (!createSnapshotAllowed("single")) {
        return nullptr;
    }

    return create(FsSnapshot::SnapshotType::Single, description, nullptr, cleanup, important);
}

/**
 * @brief Preスナップショットを作成
 *
 * 変更前のスナップショット (Pre)を作成します。
 * DISABLE_SNAPSHOTS環境変数により無効化されている場合はnullptrを返します。
 *
 * @param description スナップショットの説明
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 作成されたスナップショット、失敗時はnullptr
 */
FsSnapshot* SnapperService::createPre(const QString &description,
                                      FsSnapshot::CleanupAlgorithm cleanup,
                                      bool important)
{
    if (!createSnapshotAllowed("around")) {
        return nullptr;
    }

    return create(FsSnapshot::SnapshotType::Pre, description, nullptr, cleanup, important);
}

/**
 * @brief Postスナップショットを作成
 *
 * 変更後のスナップショット (Post)を作成します。
 * 対応するPreスナップショットとペアになります。
 *
 * @param description スナップショットの説明
 * @param previousNumber 対応するPreスナップショットの番号
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 作成されたスナップショット、失敗時はnullptr
 */
FsSnapshot* SnapperService::createPost(const QString &description,
                                       int previousNumber,
                                       FsSnapshot::CleanupAlgorithm cleanup,
                                       bool important)
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

    return create(FsSnapshot::SnapshotType::Post, description, previous, cleanup, important);
}

/**
 * @brief すべてのスナップショットを取得
 *
 * D-Bus経由でSnapperに問い合わせ、すべてのスナップショットのリストを取得します。
 *
 * @return スナップショットのリスト
 */
QList<FsSnapshot*> SnapperService::all()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        qCCritical(snapperLog) << "D-Bus interface is not valid";
        return QList<FsSnapshot*>();
    }

    QDBusReply<QString> reply = m_dbusInterface->call("ListSnapshots");

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
 * スナップショット番号を指定して、該当するスナップショットを検索します。
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
 * D-Bus経由でスナップショットへのロールバックを実行します。
 * 成功時はrollbackCompletedシグナル、失敗時はrollbackFailedシグナルを発行します。
 *
 * @param number ロールバック先のスナップショット番号
 * @return ロールバックが成功した場合はtrue、それ以外はfalse
 */
bool SnapperService::rollback(int number)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        qCCritical(snapperLog) << "D-Bus interface is not valid";
        emit rollbackFailed(tr("D-Bus connection failed."));
        return false;
    }

    QDBusReply<bool> reply = m_dbusInterface->call("RollbackSnapshot", number);

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
    } else {
        qCWarning(snapperLog) << "Rollback to snapshot" << number << "failed";
        emit rollbackFailed(tr("Rollback operation failed."));
    }

    return success;
}

/**
 * @brief 指定されたスナップショットを削除
 *
 * D-Bus経由でスナップショットの削除を実行します。
 * 成功時はsnapshotDeletedシグナル、失敗時はsnapshotDeletionFailedシグナルを発行します。
 *
 * @param number 削除するスナップショット番号
 * @return 削除が成功した場合はtrue、それ以外はfalse
 */
bool SnapperService::deleteSnapshot(int number)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        qCCritical(snapperLog) << "D-Bus interface is not valid";
        emit snapshotDeletionFailed(number, tr("D-Bus connection failed."));
        return false;
    }

    QDBusReply<bool> reply = m_dbusInterface->call("DeleteSnapshot", number);

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
    } else {
        qCWarning(snapperLog) << "Delete snapshot" << number << "failed";
        emit snapshotDeletionFailed(number, tr("Delete operation failed."));
    }

    return success;
}

/**
 * @brief スナップショットを作成 (内部実装)
 *
 * D-Bus経由でスナップショットを作成します。
 * 作成成功時はsnapshotCreatedシグナル、失敗時はsnapshotCreationFailedシグナルを発行します。
 *
 * @param snapshotType スナップショットのタイプ
 * @param description スナップショットの説明
 * @param previous 前のスナップショット (Postタイプの場合)
 * @param cleanup クリーンアップアルゴリズム
 * @param important 重要フラグ
 * @return 作成されたスナップショット、失敗時はnullptr
 */
FsSnapshot* SnapperService::create(FsSnapshot::SnapshotType snapshotType,
                                   const QString &description,
                                   FsSnapshot *previous,
                                   FsSnapshot::CleanupAlgorithm cleanup,
                                   bool important)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        qCCritical(snapperLog) << "D-Bus interface is not valid";
        emit snapshotCreationFailed(tr("D-Bus connection failed."));
        return nullptr;
    }

    QString type = FsSnapshot::snapshotTypeToString(snapshotType);
    int preNumber = previous ? previous->number() : -1;
    QString cleanupStr = FsSnapshot::cleanupAlgorithmToString(cleanup);
    if (cleanupStr.isEmpty()) {
        cleanupStr = "none";
    }

    QDBusReply<QString> reply = m_dbusInterface->call("CreateSnapshot",
                                                      type,
                                                      description,
                                                      preNumber,
                                                      cleanupStr,
                                                      important);

    if (!reply.isValid()) {
        qCCritical(snapperLog) << "Failed to create snapshot via D-Bus:"
                               << reply.error().message();
        emit snapshotCreationFailed(tr("Failed to create snapshot: %1")
                                    .arg(reply.error().message()));
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
 * @brief ターゲットルートパスを取得
 *
 * インストール中かどうかを判定し、適切なルートパスを返します。
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
 * 現在のシステムが非スイッチインストール環境かどうかを返します。
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
 * Snapperのインストールヘルパースクリプトのステップ4を実行します。
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
 * Snapperの各種設定パラメータを設定します。
 * NUMBER_CLEANUP、NUMBER_LIMIT、TIMELINE_CREATEなどの設定を行います。
 */
void SnapperService::writeSnapperConfig()
{
    QStringList args;
    args << "--no-dbus" << "set-config"
         << "NUMBER_CLEANUP=yes"
         << "NUMBER_LIMIT=2-10"
         << "NUMBER_LIMIT_IMPORTANT=4-10"
         << "TIMELINE_CREATE=no";

    bool success = false;
    executeCommand("/usr/bin/snapper", args, success);
}

/**
 * @brief /etc/sysconfig/yast2を更新
 *
 * /etc/sysconfig/yast2ファイル内のUSE_SNAPPER設定を"yes"に更新します。
 * 設定が存在しない場合は新規追加します。
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

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(snapperLog) << "Could not open" << sysconfigPath << "for writing";
        return;
    }

    QTextStream out(&file);
    out << content;
    file.close();
}

/**
 * @brief Snapperのクォータを設定
 *
 * Snapperのクォータ機能を設定します。
 */
void SnapperService::setupSnapperQuota()
{
    QStringList args;
    args << "--no-dbus" << "setup-quota";

    bool success = false;
    executeCommand("/usr/bin/snapper", args, success);
}

/**
 * @brief CSV形式のスナップショットリストをパース
 *
 * Snapperから取得したCSV形式の出力をパースし、
 * FsSnapshotオブジェクトのリストに変換します。
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
        QVariantMap userdata;
        if (fields.size() >= 8 && !fields[7].isEmpty()) {
            QStringList userdataPairs = fields[7].split(',');
            for (const QString &pair : userdataPairs) {
                QStringList keyValue = pair.split('=');
                if (keyValue.size() == 2) {
                    userdata[keyValue[0]] = keyValue[1];
                }
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
 * 指定されたプログラムを引数付きで実行し、出力を取得します。
 * xx秒のタイムアウトが設定されています。
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
