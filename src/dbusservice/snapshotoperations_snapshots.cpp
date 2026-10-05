/**
 * @file snapshotoperations_snapshots.cpp
 * @brief スナップショット一覧と作成更新削除、設定とquota管理
 *
 * ListConfigsとListSnapshots、CreateとModifyとDeleteとRollback
 * WriteSnapperConfigとSetupQuota、IsConfiguredを扱う
 * IsConfigured以外の公開スロットは認可ゲートを通してから対応するAuthorized本体を実行する
 * IsConfiguredは設定の有無を読み取るだけのため認可を要求しない
 */

#include <QDebug>
#include <QDateTime>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include "csvrecord.h"
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <algorithm>

// 古いlibsnapper (7.x未満) には LIBSNAPPER_VERSION_AT_LEAST マクロが存在しない
#ifndef LIBSNAPPER_VERSION_AT_LEAST
#define LIBSNAPPER_VERSION_AT_LEAST(major, minor)                                            \
    ((LIBSNAPPER_VERSION_MAJOR > (major)) ||                                                 \
     (LIBSNAPPER_VERSION_MAJOR == (major) && LIBSNAPPER_VERSION_MINOR >= (minor)))
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
#include <snapper/Plugins.h>
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
/**
 * @brief snapperプラグイン実行レポートの失敗分だけを警告ログへ出す
 *
 * 正常終了のentryは記録しない
 * プラグイン失敗はスナップショット操作自体の成否を変えないため警告に留める
 *
 * @param report プラグイン実行レポート
 */
static void logPluginReport(const snapper::Plugins::Report& report)
{
    for (const auto& entry : report.entries) {
        if (entry.exit_status != 0) {
            qWarning() << "Snapper plugin" << QString::fromStdString(entry.name)
                       << "exited with status" << entry.exit_status;
        }
    }
}
#endif

/**
 * @brief Snapperが設定されているか確認
 *
 * Snapper設定が1つ以上存在するかを確認する
 * 認証は不要 (list-snapshotsと同じアクションでactiveユーザは自動許可)
 *
 * @return Snapper設定が存在する場合: true
 */
bool SnapshotOperations::IsConfigured()
{
    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        return !configList.empty();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to check if snapper is configured:" << e.what();
        return false;
    }
}

/**
 * @brief Snapper設定を書き込む
 *
 * 指定されたキー/バリューペアをSnapper設定に書き込む
 * PolicyKit認証を必要とする
 *
 * @param configName Snapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::WriteSnapperConfig(const QString &configName,
                                            const QMap<QString, QString> &settings)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    // 許可リスト外のキーや不正な値は認可前に拒否する (キーと値はエラー本文に含めない)
    if (!qsnapper::security::validateSnapperConfigSettings(settings)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid config settings"));
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.configure"),
        [this, config = *cfg, settings]() {
            return writeSnapperConfigAuthorized(config, settings);
        });
}

/**
 * @brief 認可済みのWriteSnapperConfig本体
 *
 * @param configName 検証済みSnapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::writeSnapperConfigAuthorized(
    const QString &configName, const QMap<QString, QString> &settings)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        // 設定変更前にComparisonキャッシュを無効化 (mount/Filesが古い設定状態に依存しないように)
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

        std::map<std::string, std::string> info;
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info[it.key().toStdString()] = it.value().toStdString();
        }

        snapper->setConfigInfo(info);
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to write snapper config:" << e.what();
        replyError(QDBusError::Failed, QStringLiteral("Failed to write config"));
        return false;
    }
}

/**
 * @brief Snapperのクォータを設定
 *
 * 指定されたSnapper設定のクォータ機能を設定する
 * PolicyKit認証を必要とする
 *
 * @param configName Snapper設定名
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::SetupQuota(const QString &configName)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.configure"),
        [this, config = *cfg]() { return setupQuotaAuthorized(config); });
}

/**
 * @brief 認可済みのSetupQuota本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::setupQuotaAuthorized(const QString &configName)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper->setupQuota();
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to setup quota:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to setup quota: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief 利用可能なSnapper設定名のリストを返す
 *
 * libsnapperのgetConfigs()を呼び出し、存在する全Snapper設定 (例: "root", "home") の設定名を抽出して配列で返す
 * スナップショット本体は返さない
 * PolicyKit認証 (list-snapshots) を必要とする
 *
 * @return 設定名の配列、失敗時は空配列
 */
QStringList SnapshotOperations::ListConfigs()
{
    return authorizeThen<QStringList>(
        QStringLiteral("com.presire.qsnapper.list-snapshots"),
        [this]() { return listConfigsAuthorized(); });
}

/**
 * @brief 認可済みのListConfigs本体
 * @return 設定名の配列、失敗時は空配列
 */
QStringList SnapshotOperations::listConfigsAuthorized()
{
    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        QStringList configs;
        for (const auto &ci : configList) {
#if LIBSNAPPER_VERSION_AT_LEAST(6, 0)
            configs.append(QString::fromStdString(ci.get_config_name()));
#else
            configs.append(QString::fromStdString(ci.getConfigName()));
#endif
        }
        return configs;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapper configs:" << e.what();
        return QStringList();
    }
}

/**
 * @brief 指定設定のスナップショット一覧をCSVで取得する
 *
 * 空文字列のconfigNameは"root"と解釈される
 * 呼び出し毎にSnapperインスタンスを強制再構築し、外部 (snapperd/snapper CLI等) で作成された新規スナップショットを確実に反映する
 * PolicyKit認証 (list-snapshots) を必要とする
 *
 * @param configName Snapper設定名 (空文字列時は"root")
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::ListSnapshots(const QString &configName)
{
    // resolveConfigOrFailが空文字列を "root" に正規化した上で検証する
    // 失敗時はD-Busエラー応答が送出済み
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.list-snapshots"),
        [this, config = *cfg]() { return listSnapshotsAuthorized(config); });
}

/**
 * @brief 認可済みのListSnapshots本体
 * @param configName 検証済みSnapper設定名
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::listSnapshotsAuthorized(const QString &configName)
{
    try {
        // 外部で作成・削除・変更されたスナップショットを反映するため、一覧が変わっている場合は再構築する
        snapper::Snapper *snapper = getSnapperForListing(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        return formatSnapshotToCSV(snapper);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to list snapshots: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 新しいスナップショットを作成
 *
 * 指定されたパラメータで新しいスナップショットを作成する
 * single、pre、postの3種類のタイプをサポートする
 *
 * @param type スナップショットのタイプ ("single", "pre", "post")
 * @param description スナップショットの説明
 * @param preNumber postタイプの場合の対応するpreスナップショット番号
 * @param cleanup クリーンアップアルゴリズム名
 * @param important 重要フラグ
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::CreateSnapshot(const QString &configName, const QString &type, const QString &description,
                                           int preNumber, const QString &cleanup,
                                           const QMap<QString, QString> &userdata, bool important)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.create-snapshot"),
        [this, config = *cfg, type, description, preNumber, cleanup, userdata,
         important]() {
            return createSnapshotAuthorized(config, type, description, preNumber,
                                            cleanup, userdata, important);
        });
}

/**
 * @brief 認可済みのCreateSnapshot本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::createSnapshotAuthorized(
    const QString &configName, const QString &type, const QString &description,
    int preNumber, const QString &cleanup,
    const QMap<QString, QString> &userdata, bool important)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // 格納前にレコード境界を破壊する値を拒否する理由はmodifySnapshotAuthorized()側の注記を参照すること
        if (!qsnapper::security::isRecordSafeText(description)
                || !qsnapper::security::isRecordSafeText(cleanup)) {
            replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Snapshot metadata must not contain control characters"));
            return QString();
        }
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            if (!qsnapper::security::isRecordSafeText(it.key())
                    || !qsnapper::security::isRecordSafeText(it.value())) {
                replyError(QDBusError::InvalidArgs,
                           QStringLiteral("Snapshot metadata must not contain control characters"));
                return QString();
            }
        }

        // スナップショット作成 (スナップショット一覧の変化) 前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

        snapper::SCD scd;
        scd.description = description.toStdString();
        scd.cleanup = cleanup.toStdString();
        scd.read_only = true;

        // ユーザが指定した key=value 形式のユーザデータをコピー
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            scd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

        if (important) {
            scd.userdata["important"] = "yes";
        }

        snapper::Snapshots::iterator newSnapshot;
        snapper::SnapshotType snapType = static_cast<snapper::SnapshotType>(stringToSnapshotType(type));

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif
        if (snapType == snapper::PRE) {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPreSnapshot(scd, report);
#else
            newSnapshot = snapper->createPreSnapshot(scd);
#endif
        }
        else if (snapType == snapper::POST && preNumber > 0) {
            snapper::Snapshots::const_iterator preSnap = snapper->getSnapshots().find(preNumber);
            if (preSnap == snapper->getSnapshots().end()) {
                replyError(QDBusError::Failed, "Pre-snapshot not found");
                return QString();
            }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPostSnapshot(preSnap, scd, report);
#else
            newSnapshot = snapper->createPostSnapshot(preSnap, scd);
#endif
        }
        else {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createSingleSnapshot(scd, report);
#else
            newSnapshot = snapper->createSingleSnapshot(scd);
#endif
        }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        logPluginReport(report);
#endif

        // レコード境界を破壊する値を拒否する理由はformatSnapshotToCSV()側の注記を参照すること
        const QString cleanup     = QString::fromStdString(newSnapshot->getCleanup());
        const QString description = QString::fromStdString(newSnapshot->getDescription());

        // ユーザデータを key1=value1,key2=value2形式に変換
        // 各ペアは個別のCSVフィールドであるため、formatSnapshotToCSV()と同様にペア単位でクォートする
        const std::map<std::string, std::string> &userdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        bool recordSafe = qsnapper::security::isRecordSafeText(cleanup) && qsnapper::security::isRecordSafeText(description);
        for (const auto &pair : userdata) {
            const QString key   = QString::fromStdString(pair.first);
            const QString value = QString::fromStdString(pair.second);
            if (!qsnapper::security::isRecordSafeText(key) || !qsnapper::security::isRecordSafeText(value)) {
                recordSafe = false;
                break;
            }
            userdataPairs.append(qsnapper::csv::quoteField(key + "=" + value));
        }
        if (!recordSafe) {
            // スナップショット自体は作成済みである
            // 応答だけを拒否し、偽装可能なCSVをクライアントへ渡さない
            qWarning() << "Rejected snapshot creation reply: snapshot" << newSnapshot->getNum()
                       << "has metadata containing control characters";
            replyError(QDBusError::Failed, QStringLiteral("Snapshot metadata contains control characters"));
            return QString();
        }

        // 新しく作成されたスナップショットのCSV情報を返す
        // クォートは表示正しさのための修正であり、isRecordSafeText()による制御文字拒否の上に重ねる層であって代替ではない
        QString csv = "number,type,pre-number,date,user,cleanup,description,userdata\n";
        csv += QString::number(newSnapshot->getNum()) + ",";
        csv += snapshotTypeToString(newSnapshot->getType()) + ",";
        csv += QString::number(newSnapshot->getPreNum()) + ",";

        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(newSnapshot->getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(newSnapshot->getUid()) + ",";
        csv += qsnapper::csv::quoteField(cleanup) + ",";
        csv += qsnapper::csv::quoteField(description) + ",";

        csv += userdataPairs.join(",");

        return csv;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to create snapshot:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to create snapshot: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 既存スナップショットのメタデータを編集
 *
 * description / cleanup algorithm / userdata を差し替える
 * 空文字列("")のdescriptionはそのまま空文字列で上書きされる
 * userdataは渡されたマップで完全に置き換わる (差分ではない)
 * PolicyKit認証を必要とする
 *
 * @param configName Snapper設定名
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文 (空文字列も可)
 * @param cleanup 新しいcleanupアルゴリズム名
 * @param userdata 新しいuserdataマップ (置換)
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::ModifySnapshot(const QString &configName, int number,
                                        const QString &description, const QString &cleanup,
                                        const QMap<QString, QString> &userdata)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.modify-snapshot"),
        [this, config = *cfg, number, description, cleanup, userdata]() {
            return modifySnapshotAuthorized(config, number, description, cleanup,
                                            userdata);
        });
}

/**
 * @brief 認可済みのModifySnapshot本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::modifySnapshotAuthorized(
    const QString &configName, int number, const QString &description,
    const QString &cleanup, const QMap<QString, QString> &userdata)
{
    // 格納前にレコード境界を破壊する値を拒否する
    // ここを通過したmetadataはListSnapshots等でそのまま連結されるため、qSnapper自身が一覧取得を不能にする値を保存しない
    // カンマは列をずらすだけでレコード境界を壊さないため対象外とする
    if (!qsnapper::security::isRecordSafeText(description) || !qsnapper::security::isRecordSafeText(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Snapshot metadata must not contain control characters"));
        return false;
    }
    for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
        if (!qsnapper::security::isRecordSafeText(it.key()) || !qsnapper::security::isRecordSafeText(it.value())) {
            replyError(QDBusError::InvalidArgs, QStringLiteral("Snapshot metadata must not contain control characters"));
            return false;
        }
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショット属性変更前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

        snapper::SMD smd;
        smd.description = description.toStdString();
        smd.cleanup     = cleanup.toStdString();
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            smd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->modifySnapshot(snapshot, smd, report);
        logPluginReport(report);
#else
        snapper->modifySnapshot(snapshot, smd);
#endif
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to modify snapshot:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to modify snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief スナップショットを削除する (D-Busスロット)
 *
 * Polkit認可は毎回 beginAuthorization()に委ねる
 * 連続削除時の再入力はpolkitのauth_admin_keep設定により、short-lived cookieで抑止される
 *
 * @param configName 設定名
 * @param number 削除対象スナップショット番号
 * @return 成功時true
 */
bool SnapshotOperations::DeleteSnapshot(const QString &configName, int number)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.delete-snapshot"),
        [this, config = *cfg, number]() {
            return deleteSnapshotAuthorized(config, number);
        });
}

/**
 * @brief 認可済みのDeleteSnapshot本体
 *
 * @param configName 検証済み設定名
 * @param number 削除対象スナップショット番号
 * @return 成功時true
 */
bool SnapshotOperations::deleteSnapshotAuthorized(const QString &configName,
                                                  int number)
{
    resetIdleTimer();

    // 実行中の復元計画が復元元として参照しているスナップショットは削除しない
    // pin済みdirfdがソース読み取りの同一性を保証するが、復元中の削除は「認可時に存在した復元元」の消失につながるため、ここで明示的に拒否する
    for (const RestoreExecution &executionItem : m_restoreExecutions) {
        if (executionItem.configName == configName && executionItem.snapshotNumber == number) {
            replyError(QDBusError::Failed, QStringLiteral("Snapshot is in use by an active restore plan"));
            return false;
        }
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショット削除 (スナップショット一覧の変化) 前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->deleteSnapshot(snapshot, report);
        logPluginReport(report);
#else
        snapper->deleteSnapshot(snapshot);
#endif
        resetIdleTimer();   // 長時間削除後もタイマリセット
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to delete snapshot:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        resetIdleTimer();   // 例外時もタイマリセット
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時にそのスナップショットの状態で起動する
 *
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時: true、失敗時: false
 */
bool SnapshotOperations::RollbackSnapshot(const QString &configName, int number)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    // 0は現在のシステム (current) を表し、ロールバック先にはできない (snapper CLIと同じ)
    if (number <= 0) {
        replyError(QDBusError::InvalidArgs,
                   QStringLiteral("Invalid snapshot number"));
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.rollback-snapshot"),
        [this, config = *cfg, number]() {
            return rollbackSnapshotAuthorized(config, number);
        });
}

/**
 * @brief 認可済みのRollbackSnapshot本体
 *
 * @param configName 検証済み設定名
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時: true、失敗時: false
 */
bool SnapshotOperations::rollbackSnapshotAuthorized(const QString &configName, int number)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        // snapper CLI (cmd-rollback) と同じく、SUBVOLUMEが "/" のbtrfs設定以外では拒否する
        // 他の設定でdefault subvolumeを切り替えると、rootファイルシステムの起動対象を壊すため
        QString normalizedSubvolume;
        std::string fstype;
        snapper->getConfigInfo().get_value("FSTYPE", fstype);
        if (!qsnapper::restore::normalizeRestoreSubvolume(
                QString::fromStdString(snapper->subvolumeDir()), &normalizedSubvolume)
                || normalizedSubvolume != QStringLiteral("/")
                || fstype != "btrfs") {
            replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Rollback is only supported for the root btrfs config"));
            return false;
        }

        snapper::Snapshots &snapshots = snapper->getSnapshots();
        snapper::Snapshots::iterator target = snapshots.find(number);
        if (target == snapshots.end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // ロールバックはスナップショット一覧・現在状態を変化させるためComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

        // "sudo snapper rollback N"と同等の挙動を再現する
        //
        // CLI (client/snapper/cmd-rollback.cc) はambitを以下で判定する:
        //   - previous_defaultがread-only --> TRANSACTIONAL
        //     (新規スナップショット作成なしで対象を直接default化)
        //   - previous_defaultがwritable --> CLASSIC
        //       (1) 現在状態のread-onlyバックアップsnapshotを作成
        //       (2) 対象Nのwritable copy snapshotを作成
        //       (3) previous_defaultにcleanupが空なら"number"を付与
        //       (4) (2)で作成したwritable copyをdefaultに設定
        snapper::Snapshots::iterator previousDefault = snapshots.getDefault();
        const bool transactional =
            (previousDefault != snapshots.end() && previousDefault->isReadOnly());

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif

        if (transactional) {
            // TRANSACTIONAL: 対象スナップショットをそのままdefaultにする
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            target->setDefault(report);
            logPluginReport(report);
#else
            target->setDefault();
#endif
        }
        else {
            // CLASSIC: backup + writable copyを作成して、writable copyをdefaultにする

            const int prevNum =
                (previousDefault != snapshots.end()) ? static_cast<int>(previousDefault->getNum()) : -1;

            // (1) 現在状態のread-onlyバックアップ
            snapper::SCD scd1;
            scd1.description = (prevNum >= 0)
                ? std::string("rollback backup of #") + std::to_string(prevNum)
                : std::string("rollback backup");
            scd1.cleanup = "number";
            scd1.userdata["important"] = "yes";
            scd1.read_only = true;

            // (2) 対象Nのwritable copy
            snapper::SCD scd2;
            scd2.description = std::string("writable copy of #") + std::to_string(number);
            scd2.cleanup.clear();
            scd2.read_only = false;

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1, report);
            logPluginReport(report);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2, report);
            logPluginReport(report);

            // (3) previous_defaultにcleanupが空なら"number"を付与
            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd, report);
                logPluginReport(report);
            }

            // (4) writable copyをdefaultに
            writableCopy->setDefault(report);
            logPluginReport(report);
#else
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2);

            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd);
            }

            writableCopy->setDefault();
#endif
            (void)backup;
        }

        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to rollback snapshot:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to rollback snapshot: %1").arg(e.what()));
        return false;
    }
}
