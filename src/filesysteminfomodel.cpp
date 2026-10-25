/**
 * @file filesysteminfomodel.cpp
 * @brief Btrfsの使用量とサブボリューム一覧を提供するViewModelの実装
 */

#include "filesysteminfomodel.h"
#include "snapperservice.h"
#include "csvrecord.h"
#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QStringList>
#include <utility>

namespace {
    // BTRFS_IOC_SPACE_INFOのフラグ (サーバ側の定義と一致させる)
    constexpr qulonglong FLAG_DATA = 1ULL << 0;         // データチャンク
    constexpr qulonglong FLAG_SYSTEM = 1ULL << 1;       // システムチャンク
    constexpr qulonglong FLAG_METADATA = 1ULL << 2;     // メタデータチャンク
    constexpr qulonglong FLAG_RAID0 = 1ULL << 3;        // RAID0
    constexpr qulonglong FLAG_RAID1 = 1ULL << 4;        // RAID1
    constexpr qulonglong FLAG_DUP = 1ULL << 5;          // DUP
    constexpr qulonglong FLAG_RAID10 = 1ULL << 6;       // RAID10
    constexpr qulonglong FLAG_RAID5 = 1ULL << 7;        // RAID5
    constexpr qulonglong FLAG_RAID6 = 1ULL << 8;        // RAID6
    constexpr qulonglong FLAG_RAID1C3 = 1ULL << 9;      // RAID1C3
    constexpr qulonglong FLAG_RAID1C4 = 1ULL << 10;     // RAID1C4
    constexpr qulonglong FLAG_GLOBAL_RSV = 1ULL << 49;  // メタデータのグローバル予約

    // サブボリューム一覧のCSVで要求する列数
    constexpr int SUBVOLUME_COLUMN_COUNT = 6;

    /**
     * @brief CSVをRFC 4180の引用符を考慮してレコードへ分割する
     * @param csv 分割するCSV全体
     * @return レコードのリスト、引用符が閉じていない場合は空のリスト
     * @note 引用符で囲まれたフィールド内の改行はレコードの区切りとして扱わない
     *       サーバはパスに改行を含めないが、解析側でも引用符の状態を保って壊れた入力を拒否する
     */
    QStringList splitCsvRecords(const QString &csv)
    {
        QStringList records;
        QString current;
        bool inQuotes = false;
        bool atFieldStart = true;

        for (int i = 0; i < csv.size(); ++i) {
            const QChar ch = csv.at(i);
            if (inQuotes) {
                if (ch == QLatin1Char('"')) {
                    if (i + 1 < csv.size() && csv.at(i + 1) == QLatin1Char('"')) {
                        current += QLatin1String("\"\"");
                        ++i;
                    }
                    else {
                        inQuotes = false;
                        current += ch;
                    }
                }
                else {
                    current += ch;
                }
                continue;
            }

            if (ch == QLatin1Char(',')) {
                current += ch;
                atFieldStart = true;
            }
            else if (ch == QLatin1Char('\n')) {
                if (current.endsWith(QLatin1Char('\r'))) {
                    current.chop(1);
                }
                if (!current.isEmpty()) {
                    records.append(current);
                }
                current.clear();
                atFieldStart = true;
            }
            else if (ch == QLatin1Char('"') && atFieldStart) {
                inQuotes = true;
                current += ch;
                atFieldStart = false;
            }
            else {
                current += ch;
                atFieldStart = false;
            }
        }

        if (inQuotes) {
            return {};
        }

        if (current.endsWith(QLatin1Char('\r'))) {
            current.chop(1);
        }
        if (!current.isEmpty()) {
            records.append(current);
        }
        return records;
    }

    /**
     * @brief 空白を含まない10進数の符号なし整数を解析する
     * @param text 解析する文字列
     * @param value 解析結果の格納先
     * @return 解析できた場合はtrue
     */
    bool parseUnsigned(const QString &text, qulonglong *value)
    {
        if (text.isEmpty() || text != text.trimmed()) {
            return false;
        }
        bool ok = false;
        const qulonglong parsed = text.toULongLong(&ok);
        if (!ok) {
            return false;
        }
        *value = parsed;
        return true;
    }

    /**
     * @brief 空白を含まない10進数の符号付き整数を解析する
     * @param text 解析する文字列
     * @param value 解析結果の格納先
     * @return 解析できた場合はtrue
     */
    bool parseSigned(const QString &text, qlonglong *value)
    {
        if (text.isEmpty() || text != text.trimmed()) {
            return false;
        }
        bool ok = false;
        const qlonglong parsed = text.toLongLong(&ok);
        if (!ok) {
            return false;
        }
        *value = parsed;
        return true;
    }

    /**
     * @brief チャンクの種別フラグを表示用のラベルへ変換する
     * @param flags BTRFS_IOC_SPACE_INFOのフラグ
     * @return 種別の表示名
     * @note DATA|METADATAは混在チャンクであり、Global reserveはメタデータの一部として別の名前で示す
     */
    QString typeLabel(qulonglong flags)
    {
        if (flags & FLAG_GLOBAL_RSV) {
            return QCoreApplication::translate("FilesystemInfoModel", "Global reserve");
        }

        const bool data = flags & FLAG_DATA;
        const bool metadata = flags & FLAG_METADATA;
        const bool system = flags & FLAG_SYSTEM;

        if (data && metadata && !system) {
            return QCoreApplication::translate("FilesystemInfoModel", "Mixed");
        }

        QStringList parts;
        if (data) {
            parts.append(QCoreApplication::translate("FilesystemInfoModel", "Data"));
        }
        if (metadata) {
            parts.append(QCoreApplication::translate("FilesystemInfoModel", "Metadata"));
        }
        if (system) {
            parts.append(QCoreApplication::translate("FilesystemInfoModel", "System"));
        }
        if (parts.isEmpty()) {
            return QCoreApplication::translate("FilesystemInfoModel", "Unknown");
        }
        return parts.join(QLatin1Char('+'));
    }

    /**
     * @brief チャンクのプロファイルフラグを表示用のラベルへ変換する
     * @param flags BTRFS_IOC_SPACE_INFOのフラグ
     * @return プロファイルの表示名、プロファイル指定が無い場合はsingle
     * @note プロファイル名はbtrfsの用語をそのまま使うため翻訳しない
     */
    QString profileLabel(qulonglong flags)
    {
        QStringList parts;
        if (flags & FLAG_RAID0) {
            parts.append(QStringLiteral("RAID0"));
        }
        if (flags & FLAG_RAID1) {
            parts.append(QStringLiteral("RAID1"));
        }
        if (flags & FLAG_DUP) {
            parts.append(QStringLiteral("DUP"));
        }
        if (flags & FLAG_RAID10) {
            parts.append(QStringLiteral("RAID10"));
        }
        if (flags & FLAG_RAID5) {
            parts.append(QStringLiteral("RAID5"));
        }
        if (flags & FLAG_RAID6) {
            parts.append(QStringLiteral("RAID6"));
        }
        if (flags & FLAG_RAID1C3) {
            parts.append(QStringLiteral("RAID1C3"));
        }
        if (flags & FLAG_RAID1C4) {
            parts.append(QStringLiteral("RAID1C4"));
        }
        if (parts.isEmpty()) {
            return QStringLiteral("single");
        }
        return parts.join(QLatin1Char('+'));
    }
}

/**
 * @brief FilesystemInfoModelを構築する
 * @param parent Qt親オブジェクト
 */
FilesystemInfoModel::FilesystemInfoModel(QObject *parent)
    : QObject(parent)
    , m_snapperService(SnapperService::instance())
    , m_usageBusy(false)
    , m_subvolumesBusy(false)
    , m_usageLoaded(false)
    , m_subvolumesTruncated(false)
    , m_hideSnapperSnapshots(true)
    , m_deviceTotal(0)
    , m_deviceAllocated(0)
    , m_nodesize(0)
    , m_sectorsize(0)
    , m_usageGeneration(0)
    , m_subvolumeGeneration(0)
{
}

/**
 * @brief 使用量の対象となる設定名を変更する
 * @param name 新しい設定名
 * @note 変更前の設定に対する応答を捨てるため、世代番号を進めて状態を破棄する
 */
void FilesystemInfoModel::setConfigName(const QString &name)
{
    if (m_configName == name) {
        return;
    }

    m_configName = name;

    // 古い設定の応答が新しい設定の表示へ混ざらないよう、要求の世代を進める
    ++m_usageGeneration;
    ++m_subvolumeGeneration;

    // 古い要求の応答は世代の不一致で捨てられ、busyを解除する機会が無くなるため、ここで解除して新しい設定で再取得できるようにする
    setUsageBusy(false);
    setSubvolumesBusy(false);

    clearUsage();
    clearSubvolumes();
    emit configNameChanged();
}

/**
 * @brief snapperのスナップショットを一覧から隠すかを変更する
 * @param hide trueでsnapperのスナップショットを隠す
 * @note 取得済みの一覧を絞り込むだけであり、特権サービスへは問い合わせない
 */
void FilesystemInfoModel::setHideSnapperSnapshots(bool hide)
{
    if (m_hideSnapperSnapshots == hide) {
        return;
    }
    m_hideSnapperSnapshots = hide;
    emit hideSnapperSnapshotsChanged();
    rebuildVisibleSubvolumes();
}

/**
 * @brief 使用量を非同期に取得する
 * @note 設定名が空の場合は何もしない
 *       古い応答は世代番号の不一致で捨てる
 *       失敗時はusageLoadFailedを発行し、古い値を実際の状態として表示しないよう消去する
 */
void FilesystemInfoModel::refreshUsage()
{
    if (m_configName.isEmpty()) {
        return;
    }

    const quint64 generation = ++m_usageGeneration;
    setUsageBusy(true);

    auto *watcher = new QDBusPendingCallWatcher(
        m_snapperService->asyncGetFilesystemUsage(m_configName), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation]() {
        watcher->deleteLater();

        // 新しい要求または設定変更が起きている場合は、古い応答を無視する
        if (generation != m_usageGeneration) {
            return;
        }

        setUsageBusy(false);

        QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            clearUsage();
            emit usageLoadFailed(SnapperService::errorText(reply.error()));
            return;
        }

        QString error;
        if (!parseUsageCsv(reply.value(), &error)) {
            clearUsage();
            emit usageLoadFailed(error);
        }
    });
}

/**
 * @brief サブボリューム一覧を非同期に取得する
 * @note 認証を伴うため、画面から明示的に要求された場合だけ呼び出す
 *       古い応答は世代番号の不一致で捨てる
 *       失敗時はsubvolumesLoadFailedを発行し、古い一覧を実際の状態として表示しないよう消去する
 */
void FilesystemInfoModel::refreshSubvolumes()
{
    if (m_configName.isEmpty()) {
        return;
    }

    const quint64 generation = ++m_subvolumeGeneration;
    setSubvolumesBusy(true);

    auto *watcher = new QDBusPendingCallWatcher(
        m_snapperService->asyncListSubvolumes(m_configName), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation]() {
        watcher->deleteLater();

        // 新しい要求または設定変更が起きている場合は、古い応答を無視する
        if (generation != m_subvolumeGeneration) {
            return;
        }

        setSubvolumesBusy(false);

        QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            clearSubvolumes();
            emit subvolumesLoadFailed(SnapperService::errorText(reply.error()));
            return;
        }

        QString error;
        if (!parseSubvolumesCsv(reply.value(), &error)) {
            clearSubvolumes();
            emit subvolumesLoadFailed(error);
        }
    });
}

/**
 * @brief GetFilesystemUsageのCSVを解析してメンバへ反映する
 * @param csv サーバが返したCSV
 * @param error 解析に失敗した理由の格納先
 * @return すべての行を解析できた場合はtrue
 * @note 途中まで解析できても反映しない (古い値と新しい値が混ざった表示を避けるため)
 */
bool FilesystemInfoModel::parseUsageCsv(const QString &csv, QString *error)
{
    const QStringList records = splitCsvRecords(csv);
    if (records.isEmpty()) {
        *error = tr("The filesystem usage data from the service is empty or malformed.");
        return false;
    }

    bool headerSeen = false;
    bool infoSeen = false;
    QVariantList usage;
    QVariantList devices;
    qint64 deviceTotal = 0;
    qint64 deviceAllocated = 0;
    qint64 nodesize = 0;
    qint64 sectorsize = 0;

    for (const QString &record : records) {
        const QStringList fields = qsnapper::csv::splitRecord(record);
        if (fields.isEmpty()) {
            *error = tr("The filesystem usage data from the service is malformed.");
            return false;
        }

        if (!headerSeen) {
            if (fields != QStringList({QStringLiteral("kind"), QStringLiteral("flags"),
                                       QStringLiteral("total"), QStringLiteral("used")})) {
                *error = tr("The filesystem usage data from the service has an unexpected header.");
                return false;
            }
            headerSeen = true;
            continue;
        }

        if (fields.size() != 4) {
            *error = tr("The filesystem usage data from the service is malformed.");
            return false;
        }

        const QString &kind = fields.at(0);

        if (kind == QLatin1String("info")) {
            // info行はinfo,<nodesize>,<sectorsize>,<numDevices>の順で返される
            qulonglong infoNodesize = 0;
            qlonglong infoSectorsize = 0;
            qlonglong numDevices = 0;
            if (!parseUnsigned(fields.at(1), &infoNodesize)
                    || !parseSigned(fields.at(2), &infoSectorsize)
                    || !parseSigned(fields.at(3), &numDevices)) {
                *error = tr("The filesystem usage data from the service contains invalid numbers.");
                return false;
            }
            if (infoSeen || infoNodesize == 0 || infoSectorsize <= 0 || numDevices < 0) {
                *error = tr("The filesystem usage data from the service is malformed.");
                return false;
            }
            nodesize = static_cast<qint64>(infoNodesize);
            sectorsize = infoSectorsize;
            infoSeen = true;
        }
        else if (kind == QLatin1String("space")) {
            qulonglong flags = 0;
            qlonglong total = 0;
            qlonglong used = 0;
            if (!parseUnsigned(fields.at(1), &flags)
                    || !parseSigned(fields.at(2), &total)
                    || !parseSigned(fields.at(3), &used)
                    || total < 0 || used < 0) {
                *error = tr("The filesystem usage data from the service contains invalid numbers.");
                return false;
            }
            QVariantMap entry;
            entry.insert(QStringLiteral("typeLabel"), typeLabel(flags));
            entry.insert(QStringLiteral("profileLabel"), profileLabel(flags));
            entry.insert(QStringLiteral("total"), total);
            entry.insert(QStringLiteral("used"), used);
            usage.append(entry);
        }
        else if (kind == QLatin1String("device")) {
            qulonglong devid = 0;
            qlonglong size = 0;
            qlonglong allocated = 0;
            if (!parseUnsigned(fields.at(1), &devid)
                    || !parseSigned(fields.at(2), &size)
                    || !parseSigned(fields.at(3), &allocated)
                    || size < 0 || allocated < 0) {
                *error = tr("The filesystem usage data from the service contains invalid numbers.");
                return false;
            }
            QVariantMap entry;
            entry.insert(QStringLiteral("devid"), QVariant::fromValue<qulonglong>(devid));
            entry.insert(QStringLiteral("size"), size);
            entry.insert(QStringLiteral("allocated"), allocated);
            devices.append(entry);
            deviceTotal += size;
            deviceAllocated += allocated;
        }
        else {
            *error = tr("The filesystem usage data from the service contains an unknown row.");
            return false;
        }
    }

    if (!headerSeen || !infoSeen) {
        *error = tr("The filesystem usage data from the service is incomplete.");
        return false;
    }

    m_usage = usage;
    m_devices = devices;
    m_deviceTotal = deviceTotal;
    m_deviceAllocated = deviceAllocated;
    m_nodesize = nodesize;
    m_sectorsize = sectorsize;
    m_usageLoaded = true;

    emit usageChanged();
    emit devicesChanged();
    emit deviceTotalsChanged();
    emit blockSizesChanged();
    emit usageLoadedChanged();
    return true;
}

/**
 * @brief ListSubvolumesのCSVを解析してメンバへ反映する
 * @param csv サーバが返したCSV
 * @param error 解析に失敗した理由の格納先
 * @return すべての行を解析できた場合はtrue
 * @note 途中まで解析できても反映しない (古い一覧と新しい一覧が混ざった表示を避けるため)
 *       パスは最後の列であり、RFC 4180の引用符で囲まれたカンマを1つの値として扱う
 */
bool FilesystemInfoModel::parseSubvolumesCsv(const QString &csv, QString *error)
{
    const QStringList records = splitCsvRecords(csv);
    if (records.size() < 2) {
        *error = tr("The subvolume list from the service is empty or malformed.");
        return false;
    }

    // 1行目: truncated,0|1
    const QStringList truncatedFields = qsnapper::csv::splitRecord(records.at(0));
    if (truncatedFields.size() != 2
            || truncatedFields.at(0) != QLatin1String("truncated")
            || (truncatedFields.at(1) != QLatin1String("0")
                && truncatedFields.at(1) != QLatin1String("1"))) {
        *error = tr("The subvolume list from the service has an unexpected header.");
        return false;
    }
    const bool truncated = truncatedFields.at(1) == QLatin1String("1");

    // 2行目: 列名
    const QStringList header = qsnapper::csv::splitRecord(records.at(1));
    if (header != QStringList({QStringLiteral("id"), QStringLiteral("parent_id"),
                               QStringLiteral("created"), QStringLiteral("readonly"),
                               QStringLiteral("snapper"), QStringLiteral("path")})) {
        *error = tr("The subvolume list from the service has an unexpected header.");
        return false;
    }

    QVariantList subvolumes;
    for (int i = 2; i < records.size(); ++i) {
        const QStringList fields = qsnapper::csv::splitRecord(records.at(i));
        if (fields.size() != SUBVOLUME_COLUMN_COUNT) {
            *error = tr("The subvolume list from the service is malformed.");
            return false;
        }

        qulonglong id = 0;
        qulonglong parentId = 0;
        qlonglong created = 0;
        qulonglong readOnly = 0;
        qulonglong snapper = 0;
        if (!parseUnsigned(fields.at(0), &id)
                || !parseUnsigned(fields.at(1), &parentId)
                || !parseSigned(fields.at(2), &created)
                || !parseUnsigned(fields.at(3), &readOnly)
                || !parseUnsigned(fields.at(4), &snapper)
                || created < 0
                || (readOnly != 0 && readOnly != 1)
                || (snapper != 0 && snapper != 1)) {
            *error = tr("The subvolume list from the service contains invalid values.");
            return false;
        }

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QVariant::fromValue<qulonglong>(id));
        entry.insert(QStringLiteral("parentId"), QVariant::fromValue<qulonglong>(parentId));
        entry.insert(QStringLiteral("created"), QDateTime::fromSecsSinceEpoch(created));
        entry.insert(QStringLiteral("readOnly"), readOnly == 1);
        entry.insert(QStringLiteral("snapper"), snapper == 1);
        entry.insert(QStringLiteral("path"), fields.at(5));
        subvolumes.append(entry);
    }

    m_allSubvolumes = subvolumes;
    m_subvolumesTruncated = truncated;
    rebuildVisibleSubvolumes();
    emit subvolumesTruncatedChanged();
    return true;
}

/**
 * @brief 絞り込み後のサブボリューム一覧を再構築する
 * @note hideSnapperSnapshotsがtrueの場合、snapperが作成したスナップショットを除外する
 */
void FilesystemInfoModel::rebuildVisibleSubvolumes()
{
    QVariantList visible;
    if (!m_hideSnapperSnapshots) {
        visible = m_allSubvolumes;
    }
    else {
        for (const QVariant &value : std::as_const(m_allSubvolumes)) {
            const QVariantMap entry = value.toMap();
            if (!entry.value(QStringLiteral("snapper")).toBool()) {
                visible.append(entry);
            }
        }
    }

    m_subvolumes = visible;
    emit subvolumesChanged();
}

/**
 * @brief 使用量の表示状態を破棄する
 */
void FilesystemInfoModel::clearUsage()
{
    const bool hadData = m_usageLoaded
        || !m_usage.isEmpty()
        || !m_devices.isEmpty()
        || m_deviceTotal != 0
        || m_deviceAllocated != 0
        || m_nodesize != 0
        || m_sectorsize != 0;

    m_usage.clear();
    m_devices.clear();
    m_deviceTotal = 0;
    m_deviceAllocated = 0;
    m_nodesize = 0;
    m_sectorsize = 0;
    m_usageLoaded = false;

    if (hadData) {
        emit usageChanged();
        emit devicesChanged();
        emit deviceTotalsChanged();
        emit blockSizesChanged();
        emit usageLoadedChanged();
    }
}

/**
 * @brief サブボリューム一覧の表示状態を破棄する
 */
void FilesystemInfoModel::clearSubvolumes()
{
    const bool hadData = m_subvolumesTruncated
        || !m_allSubvolumes.isEmpty()
        || !m_subvolumes.isEmpty();

    m_allSubvolumes.clear();
    m_subvolumes.clear();
    m_subvolumesTruncated = false;

    if (hadData) {
        emit subvolumesChanged();
        emit subvolumesTruncatedChanged();
    }
}

/**
 * @brief 使用量の取得中状態を更新する
 * @param busy 新しい状態
 */
void FilesystemInfoModel::setUsageBusy(bool busy)
{
    if (m_usageBusy == busy) {
        return;
    }
    m_usageBusy = busy;
    emit usageBusyChanged();
}

/**
 * @brief サブボリューム一覧の取得中状態を更新する
 * @param busy 新しい状態
 */
void FilesystemInfoModel::setSubvolumesBusy(bool busy)
{
    if (m_subvolumesBusy == busy) {
        return;
    }
    m_subvolumesBusy = busy;
    emit subvolumesBusyChanged();
}
