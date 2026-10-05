/**
 * @file restoreplanexecutor.h
 * @brief 凍結済み復元計画を小さなchunkへ分割して実行するexecutor
 *
 * D-BusやSnapperやfilesystemの詳細に依存せず
 * owner確認と状態遷移は RestoreManifestRegistry へ委譲する
 */

#ifndef QSNAPPER_RESTOREPLANEXECUTOR_H
#define QSNAPPER_RESTOREPLANEXECUTOR_H

#include "restoremanifest.h"

#include <QMap>
#include <QString>

#include <functional>

// 凍結済み復元計画の分割実行を扱う名前空間
namespace qsnapper::restore {

/**
 * @class RestorePlanExecutor
 * @brief 凍結済み復元計画を小さなchunkへ分割して実行するexecutor
 *
 * D-Bus、Snapper、filesystemから独立し、owner確認と状態遷移は
 * RestoreManifestRegistryへ委譲する
 * 実処理と通知とscheduleはcallbackとして注入するため
 * event loopを使わない単体テストでもchunk境界を観測できる
 *
 * m_registry : 実行対象manifestを所有するregistryへの参照
 * m_entryApplier : 単一entryを適用するcallback
 * m_progressSink : 進捗通知のcallback
 * m_finishedSink : 終端通知のcallback
 * m_scheduler : 次chunkを実行するscheduler
 * m_activePlans : 計画単位のschedule状態を保持するmap
 */
class RestorePlanExecutor
{
public:
    using EntryApplier = std::function<bool(const QString &id,
                                             const RestoreEntry &entry)>; // 単一entry適用callback
    using ProgressSink = std::function<void(const QString &id, int current,
                                             int total, const QString &path)>; // 進捗通知callback
    using FinishedSink = std::function<void(const QString &id,
                                             ManifestState terminal,
                                             const QString &message)>; // 終端通知callback
    using ChunkScheduler = std::function<void(std::function<void()> chunk)>; // chunk実行scheduler

    static constexpr int kEntriesPerExecutionChunk = 64; // 1chunkの最大適用件数

    /**
     * @brief registryを参照するexecutorを構築する
     * @param registry 実行対象manifestを所有するregistry
     */
    explicit RestorePlanExecutor(RestoreManifestRegistry &registry);

    // callback設定系
    /**
     * @brief 単一entryを適用するcallbackを設定する
     * @param applier manifest idとentryを受け取り、適用成功時trueを返すcallback
     */
    void setEntryApplier(EntryApplier applier);

    /**
     * @brief 進捗通知callbackを設定する
     * @param sink manifest idと完了数と総数とpathを受け取るcallback
     */
    void setProgressSink(ProgressSink sink);

    /**
     * @brief 終端通知callbackを設定する
     * @param sink manifest idと終端状態とmessageを受け取るcallback
     */
    void setFinishedSink(FinishedSink sink);

    /**
     * @brief 次のchunkを実行するschedulerを設定する
     * @param scheduler chunk関数を受け取るcallback
     *                  空なら同期実行へ戻す
     */
    void setChunkScheduler(ChunkScheduler scheduler);

    // 実行制御系
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
     * @return ownerと状態確認後にnudgeを受理した場合true
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

    // 状態参照と破棄系
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
     * @class ActivePlan
     * @brief executorが保持する計画単位のschedule状態
     *
     * owner : 開始を受理した呼び出し元owner
     * chunkScheduled : 次chunkをschedulerへ渡し済みか
     * processing : chunk実行中か
     */
    struct ActivePlan {
        QString owner;             // 開始を受理した呼び出し元owner
        bool chunkScheduled = false; // 次chunkをschedulerへ渡し済みか
        bool processing = false;   // chunk実行中か
    };

    // 内部chunk駆動系
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

    // 内部終端系
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

    RestoreManifestRegistry &m_registry;   // 実行対象manifestを所有するregistryへの参照
    EntryApplier m_entryApplier;           // 単一entryを適用するcallback
    ProgressSink m_progressSink;           // 進捗通知のcallback
    FinishedSink m_finishedSink;           // 終端通知のcallback
    ChunkScheduler m_scheduler;            // 次chunkを実行するscheduler
    QMap<QString, ActivePlan> m_activePlans; // 計画単位のschedule状態を保持するmap
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREPLANEXECUTOR_H
