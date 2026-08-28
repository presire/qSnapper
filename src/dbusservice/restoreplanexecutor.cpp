#include "restoreplanexecutor.h"

#include <utility>

namespace qsnapper::restore {

/**
 * @brief registryを参照するexecutorを構築する
 * @param registry 実行対象manifestを所有するregistry
 */
RestorePlanExecutor::RestorePlanExecutor(RestoreManifestRegistry &registry)
    : m_registry(registry)
    , m_scheduler([](std::function<void()> chunk) { chunk(); })
{
}

/**
 * @brief 単一entryを適用するcallbackを設定する
 * @param applier manifest idとentryを受け取るcallback
 */
void RestorePlanExecutor::setEntryApplier(EntryApplier applier)
{
    m_entryApplier = std::move(applier);
}

/**
 * @brief 進捗通知callbackを設定する
 * @param sink 進捗を受け取るcallback
 */
void RestorePlanExecutor::setProgressSink(ProgressSink sink)
{
    m_progressSink = std::move(sink);
}

/**
 * @brief 終端通知callbackを設定する
 * @param sink 終端状態を受け取るcallback
 */
void RestorePlanExecutor::setFinishedSink(FinishedSink sink)
{
    m_finishedSink = std::move(sink);
}

/**
 * @brief 次のchunkを実行するschedulerを設定する
 * @param scheduler chunkを受け取るcallback。空なら同期実行へ戻す
 */
void RestorePlanExecutor::setChunkScheduler(ChunkScheduler scheduler)
{
    if (scheduler) {
        m_scheduler = std::move(scheduler);
    }
    else {
        m_scheduler = [](std::function<void()> chunk) { chunk(); };
    }
}

/**
 * @brief Frozen計画をRunningへ遷移して最初のchunkを予約する
 * @param id manifest識別子
 * @param owner 呼び出し元owner
 * @param err 結果エラーの格納先
 * @return 開始を受理した場合true
 */
bool RestorePlanExecutor::start(const QString &id, const QString &owner,
                                ManifestError *err)
{
    const auto status = m_registry.status(id, owner, err);
    if (!status) {
        return false;
    }
    if (status->state == ManifestState::Completed
            || status->state == ManifestState::Failed
            || status->state == ManifestState::Cancelled) {
        setError(err, ManifestError::AlreadyTerminal);
        return false;
    }
    if (status->state != ManifestState::Frozen) {
        setError(err, ManifestError::WrongState);
        return false;
    }
    if (!m_registry.markRunning(id, owner, err)) {
        return false;
    }

    ActivePlan plan;
    plan.owner = owner;
    m_activePlans.insert(id, plan);
    scheduleChunk(id);
    setError(err, ManifestError::None);
    return true;
}

/**
 * @brief idleなRunning計画のchunk loopを再予約する
 * @param id manifest識別子
 * @param owner 呼び出し元owner
 * @param err 結果エラーの格納先
 * @return nudgeを受理した場合true
 */
bool RestorePlanExecutor::requestContinue(const QString &id,
                                          const QString &owner,
                                          ManifestError *err)
{
    const auto status = m_registry.status(id, owner, err);
    if (!status) {
        return false;
    }
    if (status->state == ManifestState::Completed
            || status->state == ManifestState::Failed
            || status->state == ManifestState::Cancelled) {
        setError(err, ManifestError::AlreadyTerminal);
        return false;
    }
    if (status->state != ManifestState::Running) {
        setError(err, ManifestError::WrongState);
        return false;
    }

    const auto active = m_activePlans.constFind(id);
    if (active == m_activePlans.cend()) {
        setError(err, ManifestError::WrongState);
        return false;
    }
    if (active->owner != owner) {
        setError(err, ManifestError::OwnerMismatch);
        return false;
    }

    if (!active->chunkScheduled && !active->processing) {
        scheduleChunk(id);
    }
    setError(err, ManifestError::None);
    return true;
}

/**
 * @brief owner確認済み非終端計画をCancelledへ遷移する
 * @param id manifest識別子
 * @param owner 呼び出し元owner
 * @param err 結果エラーの格納先
 * @return cancellationを受理した場合true
 */
bool RestorePlanExecutor::requestCancel(const QString &id,
                                        const QString &owner,
                                        ManifestError *err)
{
    const auto status = m_registry.status(id, owner, err);
    if (!status) {
        return false;
    }
    if (status->state == ManifestState::Completed
            || status->state == ManifestState::Failed
            || status->state == ManifestState::Cancelled) {
        setError(err, ManifestError::AlreadyTerminal);
        return false;
    }
    if (!m_registry.cancel(id, owner, err)) {
        return false;
    }

    const auto active = m_activePlans.constFind(id);
    if (active == m_activePlans.cend() || !active->processing) {
        finishPlan(id, ManifestState::Cancelled,
                   QStringLiteral("Restore cancelled"));
    }
    setError(err, ManifestError::None);
    return true;
}

/**
 * @brief executorが指定計画のchunk状態を保持しているか返す
 * @param id manifest識別子
 * @return active状態を保持していればtrue
 */
bool RestorePlanExecutor::isActive(const QString &id) const
{
    return m_activePlans.contains(id);
}

/**
 * @brief callbackを発火せず指定計画の予約済み実行を破棄する
 * @param id manifest識別子
 */
void RestorePlanExecutor::abandon(const QString &id)
{
    m_activePlans.remove(id);
}

/**
 * @brief 任意のエラー格納先へ値を設定する
 * @param err 結果エラーの格納先
 * @param value 設定する値
 */
void RestorePlanExecutor::setError(ManifestError *err, ManifestError value)
{
    if (err) {
        *err = value;
    }
}

/**
 * @brief active計画の次chunkを1度だけschedulerへ渡す
 * @param id manifest識別子
 */
void RestorePlanExecutor::scheduleChunk(const QString &id)
{
    auto active = m_activePlans.find(id);
    if (active == m_activePlans.end()
            || active->chunkScheduled
            || active->processing) {
        return;
    }

    active->chunkScheduled = true;
    try {
        m_scheduler([this, id]() { processChunk(id); });
    }
    catch (...) {
        active = m_activePlans.find(id);
        if (active == m_activePlans.end()) {
            return;
        }
        active->chunkScheduled = false;
        ManifestError error = ManifestError::None;
        const QString message = QStringLiteral("Failed to schedule restore work");
        // markFailedはmanifestへ失敗理由を残すために呼ぶ。
        // 記録できなかった場合 (manifest消失など) も終端通知は必ず出す
        m_registry.markFailed(id, active->owner, message, &error);
        finishPlan(id, ManifestState::Failed, message);
    }
}

/**
 * @brief 最大kEntriesPerExecutionChunk件を適用して境界状態を処理する
 * @param id manifest識別子
 */
void RestorePlanExecutor::processChunk(const QString &id)
{
    auto active = m_activePlans.find(id);
    if (active == m_activePlans.end()) {
        return;
    }

    const QString owner = active->owner;
    active->chunkScheduled = false;

    ManifestError error = ManifestError::None;
    auto before = m_registry.status(id, owner, &error);
    if (!before) {
        finishVanishedPlan(id, error);
        return;
    }
    if (before->state == ManifestState::Completed
            || before->state == ManifestState::Failed
            || before->state == ManifestState::Cancelled) {
        finishPlan(id, before->state,
                   before->state == ManifestState::Completed
                       ? QStringLiteral("Restore completed")
                       : before->state == ManifestState::Cancelled
                           ? QStringLiteral("Restore cancelled")
                           : before->lastError);
        return;
    }
    if (before->state != ManifestState::Running) {
        const QString message = QStringLiteral("Restore plan is not running");
        // markFailedはmanifestへ失敗理由を残すために呼ぶ。
        // 記録できなかった場合 (manifest消失など) も終端通知は必ず出す
        m_registry.markFailed(id, owner, message, &error);
        finishPlan(id, ManifestState::Failed, message);
        return;
    }

    active = m_activePlans.find(id);
    if (active == m_activePlans.end()) {
        return;
    }
    active->processing = true;

    const auto entries = m_registry.entriesSlice(
        id, owner, before->cursor, kEntriesPerExecutionChunk, &error);
    if (!entries || entries->isEmpty()) {
        const QString message = QStringLiteral("Restore plan has no executable entries");
        // markFailedはmanifestへ失敗理由を残すために呼ぶ。
        // 記録できなかった場合 (manifest消失など) も終端通知は必ず出す
        m_registry.markFailed(id, owner, message, &error);
        finishPlan(id, ManifestState::Failed, message);
        return;
    }

    int appliedCount = 0;
    bool applyFailed = false;
    QString failedPath;
    for (const RestoreEntry &entry : *entries) {
        if (!m_activePlans.contains(id)) {
            return;
        }

        // 単一entryの適用が長時間かかってもidle TTLが満了しないよう、
        // entryごとにmanifestのTTLを延長する
        ManifestError keepAliveError = ManifestError::None;
        if (!m_registry.keepAlive(id, owner, &keepAliveError)) {
            break;
        }

        bool applied = false;
        try {
            applied = m_entryApplier && m_entryApplier(id, entry);
        }
        catch (...) {
            applied = false;
        }

        if (!m_activePlans.contains(id)) {
            return;
        }
        if (!applied) {
            applyFailed = true;
            failedPath = entry.path;
            break;
        }
        ++appliedCount;
    }

    auto boundary = m_registry.status(id, owner, &error);
    if (!boundary) {
        finishVanishedPlan(id, error);
        return;
    }

    if (boundary->state == ManifestState::Cancelled) {
        for (int index = 0; index < appliedCount; ++index) {
            if (!m_activePlans.contains(id)) {
                return;
            }
            if (m_progressSink) {
                m_progressSink(id, before->cursor + index + 1,
                               before->totalEntries, entries->at(index).path);
            }
        }
        if (m_activePlans.contains(id)) {
            finishPlan(id, ManifestState::Cancelled,
                       QStringLiteral("Restore cancelled"));
        }
        return;
    }
    if (boundary->state != ManifestState::Running) {
        finishPlan(id, boundary->state,
                   boundary->state == ManifestState::Completed
                       ? QStringLiteral("Restore completed")
                       : boundary->lastError);
        return;
    }

    if (appliedCount > 0
            && !m_registry.advance(id, owner, appliedCount, &error)) {
        finishPlan(id, ManifestState::Failed,
                   QStringLiteral("Failed to record restore progress"));
        return;
    }

    for (int index = 0; index < appliedCount; ++index) {
        if (!m_activePlans.contains(id)) {
            return;
        }
        if (m_progressSink) {
            m_progressSink(id, before->cursor + index + 1,
                           before->totalEntries, entries->at(index).path);
        }
    }
    if (!m_activePlans.contains(id)) {
        return;
    }

    auto afterProgress = m_registry.status(id, owner, &error);
    if (!afterProgress) {
        finishVanishedPlan(id, error);
        return;
    }
    if (afterProgress->state == ManifestState::Cancelled) {
        finishPlan(id, ManifestState::Cancelled,
                   QStringLiteral("Restore cancelled"));
        return;
    }

    if (applyFailed) {
        const QString message = QStringLiteral("Failed to restore path: %1")
                                    .arg(failedPath);
        // Cancelledは直前で処理済みのため、ここに到達するのは非Cancelledのみ。
        // markFailedはmanifestへ失敗理由を残すために呼ぶが、記録できなかった
        // 場合 (manifest消失など) も終端通知は必ず出す
        m_registry.markFailed(id, owner, message, &error);
        finishPlan(id, ManifestState::Failed, message);
        return;
    }

    if (afterProgress->state == ManifestState::Completed) {
        finishPlan(id, ManifestState::Completed,
                   QStringLiteral("Restore completed"));
        return;
    }
    if (afterProgress->state != ManifestState::Running) {
        finishPlan(id, afterProgress->state, afterProgress->lastError);
        return;
    }

    active = m_activePlans.find(id);
    if (active == m_activePlans.end()) {
        return;
    }
    active->processing = false;
    scheduleChunk(id);
}

/**
 * @brief active状態を解放して終端callbackを1度だけ発火する
 * @param id manifest識別子
 * @param terminal 終端状態
 * @param message 終端理由
 */
void RestorePlanExecutor::finishPlan(const QString &id,
                                     ManifestState terminal,
                                     const QString &message)
{
    m_activePlans.remove(id);
    if (m_finishedSink) {
        m_finishedSink(id, terminal, message);
    }
}

/**
 * @brief registryから消えた計画へ必ず終端通知を出して実行を打ち切る
 * @param id manifest識別子
 * @param error registryが返したエラー
 */
void RestorePlanExecutor::finishVanishedPlan(const QString &id,
                                             ManifestError error)
{
    finishPlan(id, ManifestState::Failed,
               error == ManifestError::Expired
                   ? QStringLiteral("Restore plan expired before completion")
                   : QStringLiteral("Restore plan is no longer available"));
}

} // namespace qsnapper::restore
