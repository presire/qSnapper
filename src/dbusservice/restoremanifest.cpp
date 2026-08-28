#include "restoremanifest.h"

#include <QDateTime>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace qsnapper::restore {

RestoreManifest::RestoreManifest(QString owner, QString configName,
                                 int snapshotNumber, RestoreMode mode,
                                 QString id, qint64 creationTimeMs,
                                 qint64 ttlMs)
    : m_owner(std::move(owner))
    , m_configName(std::move(configName))
    , m_snapshotNumber(snapshotNumber)
    , m_mode(mode)
    , m_id(std::move(id))
    , m_creationTimeMs(creationTimeMs)
    , m_ttlMs(ttlMs)
    , m_lastActivityMs(creationTimeMs)
{
}

bool RestoreManifest::appendEntries(const QVector<RestoreEntry> &newEntries,
                                    ManifestError *err)
{
    if (m_state != ManifestState::Staging) {
        setError(err, ManifestError::WrongState);
        return false;
    }

    m_stagingEntries += newEntries;
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::freeze(ManifestError *err)
{
    if (m_state != ManifestState::Staging) {
        setError(err, ManifestError::WrongState);
        return false;
    }
    if (m_stagingEntries.isEmpty()) {
        setError(err, ManifestError::InvalidArgument);
        return false;
    }

    m_frozenEntries = std::make_unique<const QVector<RestoreEntry>>(
        std::move(m_stagingEntries));
    m_state = ManifestState::Frozen;
    m_lastError.clear();
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::markRunning(ManifestError *err)
{
    if (m_state != ManifestState::Frozen) {
        setError(err, ManifestError::WrongState);
        return false;
    }

    m_state = ManifestState::Running;
    m_lastError.clear();
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::advanceCursor(int count, ManifestError *err)
{
    if (m_state != ManifestState::Running) {
        setError(err, ManifestError::WrongState);
        return false;
    }
    if (count <= 0 || count > totalEntries() - m_cursor) {
        setError(err, ManifestError::InvalidArgument);
        return false;
    }

    m_cursor += count;
    if (m_cursor == totalEntries()) {
        m_state = ManifestState::Completed;
    }
    m_lastError.clear();
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::markFailed(const QString &reason, ManifestError *err)
{
    if (isTerminal()) {
        setError(err, ManifestError::AlreadyTerminal);
        return false;
    }

    m_state = ManifestState::Failed;
    m_lastError = reason;
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::cancel(ManifestError *err)
{
    if (isTerminal()) {
        setError(err, ManifestError::AlreadyTerminal);
        return false;
    }

    m_state = ManifestState::Cancelled;
    m_lastError.clear();
    touch(QDateTime::currentMSecsSinceEpoch());
    setError(err, ManifestError::None);
    return true;
}

bool RestoreManifest::isTerminal() const
{
    return m_state == ManifestState::Completed
        || m_state == ManifestState::Failed
        || m_state == ManifestState::Cancelled;
}

bool RestoreManifest::isExpired(qint64 nowMs) const
{
    // 実行中の計画はTTLで回収しない。
    // TTLは「認可前に放置されたStaging / Frozen計画」を回収するための仕組みである。
    // 既に認可されて実行中の計画へ適用すると、別クライアントのPolkitプロンプトが
    // 単一スレッドのevent loopを長時間ブロックしただけでchunkの進行が止まり、
    // keepAliveも呼ばれずTTLが満了して、進行中の復元が黙って破棄される。
    // その結果live filesystemが中途半端な状態のまま残るため、Runningは対象外とする。
    // (owner消失時はQDBusServiceWatcher経由のremoveByOwnerが確実に回収するため、
    //  TTLで回収しなくても実行中計画が滞留することはない)
    if (m_state == ManifestState::Running) {
        return false;
    }

    return nowMs - m_lastActivityMs > m_ttlMs;
}

void RestoreManifest::touch(qint64 nowMs)
{
    m_lastActivityMs = nowMs;
}

const QString &RestoreManifest::owner() const
{
    return m_owner;
}

const QString &RestoreManifest::id() const
{
    return m_id;
}

ManifestState RestoreManifest::state() const
{
    return m_state;
}

const QString &RestoreManifest::configName() const
{
    return m_configName;
}

int RestoreManifest::snapshotNumber() const
{
    return m_snapshotNumber;
}

RestoreMode RestoreManifest::mode() const
{
    return m_mode;
}

int RestoreManifest::totalEntries() const
{
    return entries().size();
}

int RestoreManifest::cursor() const
{
    return m_cursor;
}

std::optional<RestoreEntry> RestoreManifest::entryAt(int index) const
{
    if (index < 0 || index >= totalEntries()) {
        return std::nullopt;
    }
    return entries().at(index);
}

QVector<RestoreEntry> RestoreManifest::entriesSlice(int offset, int count) const
{
    if (offset < 0 || count < 0 || offset > totalEntries()) {
        return {};
    }
    return entries().mid(offset, count);
}

ManifestStatus RestoreManifest::status() const
{
    ManifestStatus result;
    result.id = m_id;
    result.state = m_state;
    result.totalEntries = totalEntries();
    result.cursor = m_cursor;
    result.processed = m_cursor;
    result.mode = m_mode;
    result.configName = m_configName;
    result.snapshotNumber = m_snapshotNumber;
    result.lastError = m_lastError;
    return result;
}

const QVector<RestoreEntry> &RestoreManifest::entries() const
{
    return m_frozenEntries ? *m_frozenEntries : m_stagingEntries;
}

void RestoreManifest::setError(ManifestError *err, ManifestError value)
{
    if (err) {
        *err = value;
    }
}

RestoreManifestRegistry::RestoreManifestRegistry()
    : m_clock(&RestoreManifestRegistry::defaultNowMs)
{
}

void RestoreManifestRegistry::setClock(std::function<qint64()> clock)
{
    m_clock = clock ? std::move(clock)
                    : std::function<qint64()>(&RestoreManifestRegistry::defaultNowMs);
}

void RestoreManifestRegistry::setCapacityOverridesForTesting(
    int maxEntriesPerManifest, qint64 maxPathBytesPerManifest,
    qint64 maxEntriesGlobal, qint64 maxPathBytesGlobal)
{
    m_maxEntriesPerManifest = std::clamp(maxEntriesPerManifest, 1,
                                         kMaxEntriesPerManifest);
    m_maxPathBytesPerManifest = std::clamp(maxPathBytesPerManifest,
                                           qint64(0),
                                           kMaxPathBytesPerManifest);
    m_maxEntriesGlobal = std::clamp(maxEntriesGlobal, qint64(1),
                                    kMaxEntriesGlobal);
    m_maxPathBytesGlobal = std::clamp(maxPathBytesGlobal, qint64(0),
                                      kMaxPathBytesGlobal);
}

QString RestoreManifestRegistry::createStaging(const QString &owner,
                                               const QString &configName,
                                               int snapshotNumber,
                                               RestoreMode mode,
                                               ManifestError *err)
{
    if (countForOwner(owner) >= kMaxManifestsPerOwner) {
        setError(err, ManifestError::CapacityExceeded);
        return {};
    }
    if (count() >= kMaxManifestsGlobal) {
        setError(err, ManifestError::GlobalLimit);
        return {};
    }

    QString id;
    do {
        id = QStringLiteral("rm-")
            + QUuid::createUuid().toString(QUuid::Id128);
    } while (m_manifests.find(id) != m_manifests.end());

    const qint64 nowMs = m_clock();
    ManifestRecord record;
    record.manifest = std::make_unique<RestoreManifest>(
        owner, configName, snapshotNumber, mode, id, nowMs, kDefaultTtlMs);
    m_manifests.emplace(id, std::move(record));
    setError(err, ManifestError::None);
    return id;
}

bool RestoreManifestRegistry::stageEntries(const QString &id,
                                           const QString &owner,
                                           const QStringList &paths,
                                           const QStringList &changeTypes,
                                           ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest) {
        return false;
    }
    if (manifest->state() != ManifestState::Staging) {
        setError(err, ManifestError::WrongState);
        return false;
    }
    if (paths.size() != changeTypes.size()) {
        setError(err, ManifestError::InvalidArgument);
        return false;
    }
    if (paths.size() > kMaxEntriesPerStageChunk) {
        setError(err, ManifestError::CapacityExceeded);
        return false;
    }
    if (qint64(manifest->totalEntries()) + paths.size()
        > m_maxEntriesPerManifest) {
        setError(err, ManifestError::CapacityExceeded);
        return false;
    }

    ManifestRecord &record = m_manifests.at(id);
    const qint64 availablePathBytes = m_maxPathBytesPerManifest
                                   - record.pathBytes;
    qint64 additionalPathBytes = 0;
    for (const QString &path : paths) {
        const qint64 pathBytes = path.toUtf8().size();
        if (pathBytes > availablePathBytes - additionalPathBytes) {
            setError(err, ManifestError::CapacityExceeded);
            return false;
        }
        additionalPathBytes += pathBytes;
    }

    // グローバル予算。stagingは認可を要さないため、manifest単位の上限だけでは
    // 攻撃者が複数のD-Bus接続 (unique nameごとに別owner) を開いて上限を
    // 掛け算できる。プロセス全体の保持量をここで閉じる。
    // 変更を加える前に判定し、拒否時はmanifestを一切変化させない
    if (globalEntries() + paths.size() > m_maxEntriesGlobal) {
        setError(err, ManifestError::GlobalLimit);
        return false;
    }
    if (globalPathBytes() + additionalPathBytes > m_maxPathBytesGlobal) {
        setError(err, ManifestError::GlobalLimit);
        return false;
    }

    QVector<RestoreEntry> entriesToAppend;
    entriesToAppend.reserve(paths.size());
    for (qsizetype index = 0; index < paths.size(); ++index) {
        entriesToAppend.append({paths.at(index), changeTypes.at(index)});
    }
    if (!manifest->appendEntries(entriesToAppend, err)) {
        return false;
    }

    record.pathBytes += additionalPathBytes;
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::freeze(const QString &id, const QString &owner,
                                     ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest || !manifest->freeze(err)) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::markRunning(const QString &id,
                                          const QString &owner,
                                          ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest || !manifest->markRunning(err)) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::advance(const QString &id, const QString &owner,
                                      int count, ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest || !manifest->advanceCursor(count, err)) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::keepAlive(const QString &id, const QString &owner,
                                        ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::markFailed(const QString &id,
                                         const QString &owner,
                                         const QString &reason,
                                         ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest || !manifest->markFailed(reason, err)) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

bool RestoreManifestRegistry::cancel(const QString &id, const QString &owner,
                                     ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest || !manifest->cancel(err)) {
        return false;
    }
    manifest->touch(m_clock());
    return true;
}

std::optional<ManifestStatus> RestoreManifestRegistry::status(
    const QString &id, const QString &owner, ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest) {
        return std::nullopt;
    }
    setError(err, ManifestError::None);
    return manifest->status();
}

std::optional<QVector<RestoreEntry>> RestoreManifestRegistry::entriesSlice(
    const QString &id, const QString &owner, int offset, int count,
    ManifestError *err)
{
    RestoreManifest *manifest = findOwned(id, owner, err);
    if (!manifest) {
        return std::nullopt;
    }
    if (offset < 0 || count < 0 || offset > manifest->totalEntries()) {
        setError(err, ManifestError::InvalidArgument);
        return std::nullopt;
    }
    setError(err, ManifestError::None);
    return manifest->entriesSlice(offset, count);
}

int RestoreManifestRegistry::removeByOwner(const QString &owner)
{
    int removed = 0;
    for (auto it = m_manifests.begin(); it != m_manifests.end();) {
        if (it->second.manifest->owner() == owner) {
            it = m_manifests.erase(it);
            ++removed;
        }
        else {
            ++it;
        }
    }
    return removed;
}

int RestoreManifestRegistry::purgeExpired()
{
    const qint64 nowMs = m_clock();
    int removed = 0;
    for (auto it = m_manifests.begin(); it != m_manifests.end();) {
        if (it->second.manifest->isExpired(nowMs)) {
            it = m_manifests.erase(it);
            ++removed;
        }
        else {
            ++it;
        }
    }
    return removed;
}

bool RestoreManifestRegistry::remove(const QString &id)
{
    return m_manifests.erase(id) != 0;
}

int RestoreManifestRegistry::count() const
{
    return static_cast<int>(m_manifests.size());
}

int RestoreManifestRegistry::countForOwner(const QString &owner) const
{
    int result = 0;
    for (const auto &item : m_manifests) {
        if (item.second.manifest->owner() == owner) {
            ++result;
        }
    }
    return result;
}

qint64 RestoreManifestRegistry::globalPathBytes() const
{
    qint64 total = 0;
    for (const auto &item : m_manifests) {
        total += item.second.pathBytes;
    }
    return total;
}

qint64 RestoreManifestRegistry::globalEntries() const
{
    qint64 total = 0;
    for (const auto &item : m_manifests) {
        total += item.second.manifest->totalEntries();
    }
    return total;
}

RestoreManifest *RestoreManifestRegistry::findOwned(const QString &id,
                                                    const QString &owner,
                                                    ManifestError *err)
{
    const auto it = m_manifests.find(id);
    if (it == m_manifests.end()) {
        setError(err, ManifestError::NotFound);
        return nullptr;
    }

    RestoreManifest *manifest = it->second.manifest.get();
    if (manifest->owner() != owner) {
        setError(err, ManifestError::OwnerMismatch);
        return nullptr;
    }
    if (manifest->isExpired(m_clock())) {
        m_manifests.erase(it);
        setError(err, ManifestError::Expired);
        return nullptr;
    }

    setError(err, ManifestError::None);
    return manifest;
}

void RestoreManifestRegistry::setError(ManifestError *err,
                                       ManifestError value)
{
    if (err) {
        *err = value;
    }
}

qint64 RestoreManifestRegistry::defaultNowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace qsnapper::restore
