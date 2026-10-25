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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Failed to check if snapper is configured";
        return false;
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to check if snapper is configured: unexpected exception";
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
 * 共有のSnapperインスタンスのsetConfigInfo()は使わず、設定ファイルをその場で読み直したConfigInfoへ書き込む
 *   - 共有のインスタンスは同じ名前の設定を再利用するため、設定が削除・再作成されていると、古い内容で上書きしてしまう
 *   - setConfigInfo()はTIMELINE_CREATE=yesのときにsystemctlを起動してtimerを有効にする
 *     本サービスは外部コマンドを起動せず、timerの操作は専用の認可 (manage-timers) を通すSetTimerEnabledに分ける
 * 書き込む前に比較キャッシュ、一覧の指紋、共有のインスタンスの順に破棄し、古い設定に基づく結果を再利用させない
 *
 * @param configName 検証済みSnapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時: true、失敗時: false
 * @note 認可を待つ間に入力が変わることはないが、検証を本体の直前にもう一度行い、検証を通らない値を決して書き込まない
 */
bool SnapshotOperations::writeSnapperConfigAuthorized(
    const QString &configName, const QMap<QString, QString> &settings)
{
    if (!qsnapper::security::validateConfigName(configName)
            || !qsnapper::security::validateSnapperConfigSettings(settings)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid config settings"));
        return false;
    }

    // 共通ゲートで確認済みだが、共有のインスタンスを破棄する直前にも復元の実行中でないことを確かめる
    if (rejectWhileRestoreRunning()) {
        return false;
    }

    m_comparisonCache.clear();
    m_snapshotListFingerprint.clear();
    m_snapper.reset();
    m_currentConfig.clear();

    try {
        // 設定ファイルが存在しない場合はコンストラクタが例外を送出するため、新しい設定ファイルを作ることはない
        snapper::ConfigInfo info(configName.toStdString(), "/");
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info.set_value(it.key().toStdString(), it.value().toStdString());
        }
        // デストラクタによる暗黙の保存は失敗を握りつぶすため、明示的に保存して失敗を検出する
        info.save();
        return true;
    }
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Failed to write snapper config";
    }
    catch (const std::exception &) {
        qWarning() << "Failed to write snapper config";
    }
    replyError(QDBusError::Failed, QStringLiteral("Failed to write config"));
    return false;
}

/**
 * @brief Snapperのクォータを設定
 *
 * 指定されたSnapper設定のクォータ機能を設定する
 * qgroupの作成と設定ファイルのQGROUPの書き換えを伴い、設定値の編集とは影響の範囲が異なるため、
 * configureとは別の専用のアクション (setup-quota) で認可する
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
        QStringLiteral("com.presire.qsnapper.setup-quota"),
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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to setup quota";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        return false;
    }
    catch (const std::exception &) {
        // libsnapper以外の例外 (メモリ不足など) でも、内容を載せずに固定の文言で失敗を返す
        qWarning() << "Failed to setup quota";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
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
 * @note 共有のSnapperインスタンス・比較キャッシュ・mount状態に触れず、静的なgetConfigs()だけを使うため、
 *       復元の実行中も許可する (復元中もGUIが設定の一覧を表示し続けられるようにするため)
 */
QStringList SnapshotOperations::ListConfigs()
{
    return authorizeThen<QStringList>(
        QStringLiteral("com.presire.qsnapper.list-snapshots"),
        [this]() { return listConfigsAuthorized(); },
        RestoreGatePolicy::AllowWhileRestoring);
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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Failed to list snapper configs";
        return QStringList();
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to list snapper configs: unexpected exception";
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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to list snapshots";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        return QString();
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to list snapshots: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Internal error"));
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

    // 不正な値のために認可のプロンプトを出さないよう、認可の前にも判定する
    if (!qsnapper::security::isAllowedCreateCleanupAlgorithm(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid cleanup algorithm"));
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
    // 即時許可と遅延許可のどちらの経路から本体へ入っても、検証を通らない値を保存しないよう、本体の先頭でも判定する
    if (!qsnapper::security::isAllowedCreateCleanupAlgorithm(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid cleanup algorithm"));
        return QString();
    }

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
        const QString createdCleanup     = QString::fromStdString(newSnapshot->getCleanup());
        const QString createdDescription = QString::fromStdString(newSnapshot->getDescription());

        // ユーザデータを key1=value1,key2=value2形式に変換
        // 各ペアは個別のCSVフィールドであるため、formatSnapshotToCSV()と同様にペア単位でクォートする
        const std::map<std::string, std::string> &createdUserdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        bool recordSafe = qsnapper::security::isRecordSafeText(createdCleanup) && qsnapper::security::isRecordSafeText(createdDescription);
        for (const auto &pair : createdUserdata) {
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
        csv += qsnapper::csv::quoteField(createdCleanup) + ",";
        csv += qsnapper::csv::quoteField(createdDescription) + ",";

        csv += userdataPairs.join(",");

        return csv;

    }
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to create snapshot";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        return QString();
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to create snapshot: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Internal error"));
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
 * @param cleanup 新しいcleanupアルゴリズム名 (number / timeline / empty-pre-post / 空文字列のいずれか)
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

    // 不正な値のために認可のプロンプトを出さないよう、認可の前にも判定する
    // 未知の名前を保存すると、そのスナップショットがcleanupの対象から黙って外れるため、作成と同じ許可リストで拒否する
    if (!qsnapper::security::isAllowedCreateCleanupAlgorithm(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid cleanup algorithm"));
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
    // 即時許可と遅延許可のどちらの経路から本体へ入っても、検証を通らない値を保存しないよう、本体の先頭でも判定する
    if (!qsnapper::security::isAllowedCreateCleanupAlgorithm(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid cleanup algorithm"));
        return false;
    }

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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to modify snapshot";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        return false;
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to modify snapshot: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Internal error"));
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

    // 認可待ち (Frozen) の計画が参照するスナップショットも削除しない
    // 管理者が確認した復元元や比較相手が、認可の直前に消えたり別の番号の内容に変わったりしないようにするため
    // Staging計画は誰でも作れるため、削除を止める理由にしない
    // 期限切れの計画は先に終端させ、放置された計画で削除を止め続けないようにする
    purgeExpiredRestorePlans();
    if (m_restoreRegistry.isSnapshotReferencedByActivePlan(configName, number)) {
        replyError(QDBusError::Failed, QStringLiteral("Snapshot is in use by an active restore plan"));
        return false;
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

        // 同じ番号が後で別のスナップショットに使われたときに、古い作成中の計画がその内容を指して確定されないよう破棄する
        discardStagingRestorePlans(configName, number);

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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to delete snapshot";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        resetIdleTimer();   // 例外時もタイマリセット
        return false;
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to delete snapshot: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Internal error"));
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
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログにもD-Busのエラー本文にも載せない
        qWarning() << "Failed to rollback snapshot";
        replyError(QDBusError::Failed, QStringLiteral("Snapper reported an error"));
        return false;
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外もスロットの外へ出すとサービスが異常終了するため、固定の文言で応答する
        qWarning() << "Failed to rollback snapshot: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Internal error"));
        return false;
    }
}
