#ifndef QSNAPPER_RESTOREPLANEXECUTOR_H
#define QSNAPPER_RESTOREPLANEXECUTOR_H

#include "restoremanifest.h"

#include <QMap>
#include <QString>

#include <functional>

namespace qsnapper::restore {

/**
 * @brief 凍結済み復元計画を小さなchunkへ分割して実行するexecutor
 *
 * D-Bus、Snapper、filesystemから独立し、owner確認と状態遷移は
 * RestoreManifestRegistryへ委譲する。実処理と通知とscheduleはcallbackとして
 * 注入するため、event loopを使わない単体テストでもchunk境界を観測できる。
 */
class RestorePlanExecutor
{
public:
    using EntryApplier = std::function<bool(const QString &id,
                                             const RestoreEntry &entry)>;
    using ProgressSink = std::function<void(const QString &id, int current,
                                             int total, const QString &path)>;
    using FinishedSink = std::function<void(const QString &id,
                                             ManifestState terminal,
                                             const QString &message)>;
    using ChunkScheduler = std::function<void(std::function<void()> chunk)>;

    static constexpr int kEntriesPerExecutionChunk = 64;

    /**
     * @brief registryを参照するexecutorを構築する
     * @param registry 実行対象manifestを所有するregistry
     */
    explicit RestorePlanExecutor(RestoreManifestRegistry &registry);

    /**
     * @brief 単一entryを適用するcallbackを設定する
     * @param applier manifest idとentryを受け取り、適用成功時trueを返すcallback
     */
    void setEntryApplier(EntryApplier applier);

    /**
     * @brief 進捗通知callbackを設定する
     * @param sink manifest id、完了数、総数、pathを受け取るcallback
     */
    void setProgressSink(ProgressSink sink);

    /**
     * @brief 終端通知callbackを設定する
     * @param sink manifest id、終端状態、messageを受け取るcallback
     */
    void setFinishedSink(FinishedSink sink);

    /**
     * @brief 次のchunkを実行するschedulerを設定する
     * @param scheduler chunk関数を受け取るcallback。空なら同期実行へ戻す
     */
    void setChunkScheduler(ChunkScheduler scheduler);

    /**
     * @brief owner確認済みFrozen計画をRunningへ遷移して最初のchunkを予約する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return 開始を受理した場合true
     */
    bool start(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief idleなRunning計画のchunk loopを再予約する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return owner・状態確認後にnudgeを受理した場合true
     */
    bool requestContinue(const QString &id, const QString &owner,
                         ManifestError *err);

    /**
     * @brief owner確認済み非終端計画をCancelledへ遷移する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return cancellationを受理した場合true
     */
    bool requestCancel(const QString &id, const QString &owner,
                       ManifestError *err);

    /**
     * @brief executorが指定計画のchunk状態を保持しているか返す
     * @param id manifest識別子
     * @return active状態を保持していればtrue
     */
    bool isActive(const QString &id) const;

    /**
     * @brief callbackを発火せず指定計画の予約済み実行を破棄する
     * @param id manifest識別子
     */
    void abandon(const QString &id);

private:
    /**
     * @brief executorが保持する計画単位のschedule状態
     */
    struct ActivePlan {
        QString owner;
        bool chunkScheduled = false;
        bool processing = false;
    };

    /**
     * @brief 任意のエラー格納先へ値を設定する
     * @param err 結果エラーの格納先
     * @param value 設定する値
     */
    static void setError(ManifestError *err, ManifestError value);

    /**
     * @brief active計画の次chunkを1度だけschedulerへ渡す
     * @param id manifest識別子
     */
    void scheduleChunk(const QString &id);

    /**
     * @brief 最大kEntriesPerExecutionChunk件を適用して境界状態を処理する
     * @param id manifest識別子
     */
    void processChunk(const QString &id);

    /**
     * @brief active状態を解放して終端callbackを1度だけ発火する
     * @param id manifest識別子
     * @param terminal 終端状態
     * @param message 終端理由
     */
    void finishPlan(const QString &id, ManifestState terminal,
                    const QString &message);

    /**
     * @brief registryから消えた計画へ必ず終端通知を出して実行を打ち切る
     * @param id manifest識別子
     * @param error registryが返したエラー
     */
    void finishVanishedPlan(const QString &id, ManifestError error);

    RestoreManifestRegistry &m_registry;
    EntryApplier m_entryApplier;
    ProgressSink m_progressSink;
    FinishedSink m_finishedSink;
    ChunkScheduler m_scheduler;
    QMap<QString, ActivePlan> m_activePlans;
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREPLANEXECUTOR_H
