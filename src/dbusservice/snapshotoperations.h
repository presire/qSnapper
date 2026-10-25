#ifndef SNAPSHOTOPERATIONS_H
#define SNAPSHOTOPERATIONS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVariant>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QPointer>
#include <QTimer>
#include <sys/stat.h>
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <snapper/Snapshot.h>
#include "comparisoncache.h"
#include "restoremanifest.h"
#include "restoreplanexecutor.h"
#include "restorevalidation.h"
#include "snapshotoperations_types.h"
#include "snappertimerstate.h"
#include "systemdtimeroperation.h"
#include "snapperdoperation.h"

namespace snapper {
    class Snapper;
    class Comparison;
}

/** @class SnapshotOperations
 *  @brief rootで動作するスナップショット操作のD-Busサービス本体
 *
 * 一覧と作成更新削除、差分参照、staged復元計画を公開スロットとして提供する
 * 特権操作はpolkit認可ゲートを通してから実行する
 */
class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    // 復元実行コンテキストと応答退避と認可結果の実体はsnapshotoperations_types.hのqsnapper::dbusserviceに置く
    // 本クラス内では従来どおり非修飾名で参照する
    using RestoreExecution = qsnapper::dbusservice::RestoreExecution;
    using CallReply = qsnapper::dbusservice::CallReply;
    using AuthorizationOutcome = qsnapper::dbusservice::AuthorizationOutcome;

    static constexpr int IdleTimeoutMs = 5 * 60 * 1000;     // アイドルタイムアウト (5分)
                                                            // 最後のD-Busメソッド呼び出しから本値を超えてアクセスが無い場合、サービスプロセスは自律的に終了する
    static constexpr int MaxPendingAuthorizationsPerUid = 4;    // 1つの呼び出し元UIDが同時に保持できるPolkitプロンプト待ちの上限
                                                                // 1ユーザが全体の枠を占有して他のユーザの操作を拒否させないようにする
    static constexpr int MaxPendingAuthorizations = 16;         // 全体で同時に保持できるPolkitプロンプト待ちの上限
                                                                // プロンプトはタイムアウトを持たないため、未応答のまま滞留し得る
                                                                // 1呼び出しあたりの保持量は小さいが、無制限に積ませない
    std::unique_ptr<snapper::Snapper> m_snapper;            // 現在のSnapperインスタンス
    ComparisonCache<snapper::Comparison> m_comparisonCache; // m_comparisonCacheは、m_snapperの後に宣言
                                                            // (デストラクション順序: Comparisonが所有するmounts/FilesがSnapperより先に破棄されるようにするため)
    QString m_currentConfig;                                // 現在選択中のSnapper設定名
    QByteArray m_snapshotListFingerprint;                   // m_snapperが読み込んだ時点の .snapshots の指紋 (空なら一覧の再利用不可)
    QTimer m_idleTimer;                                     // アイドルタイムアウト用タイマ
    qsnapper::restore::RestoreManifestRegistry m_restoreRegistry; // 復元計画マニフェストの登録簿
    qsnapper::restore::RestorePlanExecutor m_restoreExecutor; // 凍結計画の分割実行器
    QDBusServiceWatcher *m_ownerWatcher = nullptr; // owner消失監視のサービス監視子
    QMap<QString, RestoreExecution> m_restoreExecutions; // 実行中計画の固定コンテキスト
    QMap<QString, QString> m_restorePlanOwners; // 計画とownerの対応付け
    qsnapper::restore::FinishedRestorePlanStore m_finishedRestorePlans; // 終端した計画の詳細 (ownerだけがGetRestorePlanStatusで取得する)
    std::function<bool(bool *)> m_rootReadOnlyProbe;    // rootサブボリュームのread-only状態を取得する (テストで差し替え可能)
    std::function<bool()> m_rootReadWriteRestorer;      // rootサブボリュームをrwへ戻す (テストで差し替え可能)

    std::optional<CallReply> m_deferredReply;               // 遅延応答の継続を実行している間のみ有効
                                                            // replyError()がsendErrorReply()ではなくキャプチャ済みmessageを使う判断に用いる
    qsnapper::security::PendingAuthorizationBudget m_pendingAuthorizations{MaxPendingAuthorizationsPerUid, MaxPendingAuthorizations};  // polkitプロンプト待ちの件数 (呼び出し元UID単位と全体)
    QPointer<qsnapper::dbusservice::SystemdTimerOperation> m_timerOperation;   // 実行中のtimerの操作 (同時に1件だけ)
    QPointer<qsnapper::dbusservice::SnapperdOperation> m_snapperdOperation;    // snapperdへ転送した実行中の変更 (同時に1件だけ、完了まで他の操作を拒否する)

private:
    /** @brief アイドルタイマをリセットする
     */
    void resetIdleTimer();

    /** @brief 現在のD-Bus呼び出しの応答コンテキストをキャプチャする
     * @return キャプチャ済み応答コンテキスト (D-Bus経由でなければfromDBus=false)
     */
    CallReply captureCallReply() const;

    /** @brief 同期応答と遅延応答のどちらでも正しくD-Busエラーを返す
     * @param type 返すD-Busエラー種別
     * @param text エラーメッセージ
     */
    void replyError(QDBusError::ErrorType type, const QString &text)
    {
        replyError(QDBusError::errorString(type), text);
    }

    /** @brief 同期応答と遅延応答のどちらでも正しく名前付きのD-Busエラーを返す
     *  標準のエラー種別に無い本サービス固有のエラー名 (RestoreInProgressなど) を返すために使う
     * @param errorName 返すD-Busエラー名
     * @param text エラーメッセージ (攻撃者が制御し得る値を含めない固定の文言)
     */
    void replyError(const QString &errorName, const QString &text);

    /** @brief 復元の実行中、またはsnapperdへ転送した変更の実行中であれば、操作を固定の文言で拒否する
     *  共有のSnapperインスタンス、比較キャッシュ、mount状態は復元の実行中に使われ続けるため、
     *  非復元操作がそれらを差し替えたり破棄したりすると、実行中の復元が壊れる
     *  snapperdへ転送した変更 (config作成・削除、一括削除) の実行中は、configやスナップショットの一覧が変わる途中であるため、
     *  変更前の状態を前提とした操作を受け付けない
     * @return 拒否してエラー応答を送出した場合: true
     */
    bool rejectWhileRestoreRunning();

    /** @class RestoreGatePolicy
     *  @brief authorizeThenを通る操作を、復元の実行中に拒否するかどうか
     *
     * 既定は拒否であり、新しく追加した操作は自動的にゲートの対象になる
     * 許可するのは、共有のSnapperインスタンス・比較キャッシュ・mount状態に一切触れない操作だけに限る
     */
    enum class RestoreGatePolicy {
        RejectWhileRestoring,   // 復元の実行中は拒否する (既定)
        AllowWhileRestoring     // 共有状態に触れないため、復元の実行中も許可する
    };

    /** @brief polkit認可を要求する (対話が必要な場合のみ非同期化する)
     * @param actionId PolkitアクションID
     * @param continuation 認可完了時に呼ぶ継続 (キャプチャ済み応答コンテキストと可否を受け取る)
     * @return 即時許可 / 即時拒否 / 遅延のいずれか
     */
    AuthorizationOutcome beginAuthorization(const QString &actionId, std::function<void(const CallReply &, bool)> continuation);

    /** @brief 認可待ち1件の終了を記録し、必要ならアイドルタイマを再開する
     * @param callerUid 認可待ちを開始したときの呼び出し元UID
     */
    void endPendingAuthorization(uint callerUid);

    /** @brief 遅延応答経路で本体を実行し、戻り値をD-Bus応答として送出する
     *  本体が既にreplyError()でエラーを返している場合は値応答を送らない
     *  @tparam T 対象D-Busメソッドの戻り値型
     * @param reply キャプチャ済み応答コンテキスト
     * @param body 認可済みの本体処理
     * @param granted 認可されたか
     * @param gatePolicy 復元の実行中に拒否するかどうか
     * @note polkitプロンプトを待つ間に復元が始まり得るため、認可前の確認だけでは足りない
     *       本体を実行する直前に、復元の実行中でないことを再確認する
     */
    template <typename T, typename Body>
    void completeDeferredCall(const CallReply &reply, const Body &body, bool granted,
                              RestoreGatePolicy gatePolicy)
    {
        m_deferredReply = reply;
        T result{};
        if (granted) {
            if (gatePolicy == RestoreGatePolicy::AllowWhileRestoring
                    || !rejectWhileRestoreRunning()) {
                result = body();
            }
        }
        else {
            replyError(QDBusError::AccessDenied,
                       QStringLiteral("Authorization failed"));
        }

        const bool alreadyReplied = m_deferredReply->replied;
        m_deferredReply.reset();
        if (!alreadyReplied) {
            reply.connection.send(
                reply.message.createReply(QVariant::fromValue(result)));
        }
    }

    /** @brief 認可してから本体を実行する共通ゲート
     *  対話不要ならその場で本体を同期実行して戻り値を返す (従来と同じ挙動)
     *  対話が必要な場合は遅延応答へ切り替え、既定値を返して呼び出しを終える
     *
     *  復元の実行中は、gatePolicyで許可した操作を除いて本体を実行しない
     *  確認は次の3か所で行う
     *    - polkitへの問い合わせ前 (実行できない操作のためにプロンプトを出さない)
     *    - 即時許可の経路で、本体を実行する直前
     *    - 遅延許可の経路で、本体を実行する直前 (completeDeferredCall)
     *  復元計画のメソッドはbeginAuthorizationを直接使うため、本ゲートの対象外であり、既存の排他に任せる
     *  @tparam T 対象D-Busメソッドの戻り値型
     * @param actionId PolkitアクションID
     * @param body 認可済みの本体処理
     * @param gatePolicy 復元の実行中に拒否するかどうか (既定は拒否)
     * @return 同期実行時は本体の戻り値、それ以外はT{}
     */
    template <typename T, typename Body>
    T authorizeThen(const QString &actionId, Body body,
                    RestoreGatePolicy gatePolicy = RestoreGatePolicy::RejectWhileRestoring)
    {
        const bool gated = gatePolicy == RestoreGatePolicy::RejectWhileRestoring;
        if (gated && rejectWhileRestoreRunning()) {
            return T{};
        }

        const AuthorizationOutcome outcome = beginAuthorization(
            actionId,
            [this, body, gatePolicy](const CallReply &reply, bool granted) {
                completeDeferredCall<T>(reply, body, granted, gatePolicy);
            });

        if (outcome == AuthorizationOutcome::Granted) {
            if (gated && rejectWhileRestoreRunning()) {
                return T{};
            }
            return body();
        }
        return T{};
    }

    /** @brief configNameを正規化＋検証し、不正ならD-Busエラー応答を返す
     * @param configName 検査対象の設定名 (空文字列は"root"として扱う)
     * @return 正規化後の設定名 (有効な場合) で、無効でエラー応答済みならstd::nullopt
     */
    std::optional<QString> resolveConfigOrFail(const QString &configName);

    /** @brief 認可済みのListConfigs本体
     * @return 設定名の配列
     */
    QStringList listConfigsAuthorized();

    /** @brief 認可済みのListSnapshots本体
     * @param configName 検証済み設定名
     */
    QString listSnapshotsAuthorized(const QString &configName);

    /** @brief 認可済みのCreateSnapshot本体
     * @param configName 検証済み設定名
     */
     QString createSnapshotAuthorized(const QString &configName, const QString &type, const QString &description, int preNumber, const QString &cleanup, const QMap<QString, QString> &userdata, bool important);
    /** @brief 認可済みのModifySnapshot本体
     * @param configName 検証済み設定名
     */
     bool modifySnapshotAuthorized(const QString &configName, int number, const QString &description, const QString &cleanup, const QMap<QString, QString> &userdata);
    /** @brief 認可済みのDeleteSnapshot本体
     * @param configName 検証済み設定名
     */
    bool deleteSnapshotAuthorized(const QString &configName, int number);

    /** @brief 認可済みのRollbackSnapshot本体
     * @param configName 検証済み設定名
     */
    bool rollbackSnapshotAuthorized(const QString &configName, int number);

    /** @brief 認可済みのGetFileChanges本体
     * @param configName 検証済み設定名
     */
     QString getFileChangesAuthorized(const QString &configName, int snapshotNumber);
    /** @brief 認可済みのGetFileChangesBetween本体
     * @param configName 検証済み設定名
     */
     QString getFileChangesBetweenAuthorized(const QString &configName, int number1, int number2);
    /** @brief 認可済みのGetFileDiffAndDetails本体
     * @param configName 検証済み設定名
     * @param filePath 検証済み絶対パス
     */
     QString getFileDiffAndDetailsAuthorized(const QString &configName, int snapshotNumber, const QString &filePath);
    /** @brief 認可済みのGetFileDiffBetween本体
     * @param configName 検証済み設定名
     * @param filePath 検証済み絶対パス
     */
     QString getFileDiffBetweenAuthorized(const QString &configName, int number1, int number2, const QString &filePath);
    /** @brief 認可済みのWriteSnapperConfig本体
     * @param configName 検証済み設定名
     */
     bool writeSnapperConfigAuthorized(const QString &configName, const QMap<QString, QString> &settings);
    /** @brief 認可済みのSetupQuota本体
     * @param configName 検証済み設定名
     */
    bool setupQuotaAuthorized(const QString &configName);

    /** @brief 認可済みのGetConfig本体
     * @param configName 検証済み設定名
     * @return 編集できるキーとSUBVOLUME / FSTYPE / QGROUPの値 (設定ファイルに無いキーは含めない)
     */
    QMap<QString, QString> getConfigAuthorized(const QString &configName);

    /** @brief 認可済みのGetTimerStates本体
     *  応答はtimerの状態を読み終えた後に非同期で送るため、戻り値は使われない
     * @return 常に空のマップ
     */
    SnapperTimerStateMap getTimerStatesAuthorized();

    /** @brief 認可済みのSetTimerEnabled本体
     *  応答はsystemdの操作と状態の読み直しを終えた後に非同期で送るため、戻り値は使われない
     * @param unit 呼び出し元が指定したunit名 (本体で許可リストと照合し直す)
     * @param enabled trueで有効化と起動、falseで停止と無効化
     * @return 常にfalse
     */
    bool setTimerEnabledAuthorized(const QString &unit, bool enabled);

    /** @brief 現在の呼び出しの応答を、本体から戻った後に非同期で送るために確保する
     *  即時許可の経路ではQDBusContextの遅延応答に切り替え、遅延許可の経路ではcompleteDeferredCallに応答を送らせない
     * @return 確保した応答コンテキスト (D-Bus経由の呼び出しでなければstd::nullopt)
     */
    std::optional<CallReply> claimReplyForAsyncCompletion();

    /** @brief 確保した応答コンテキストへ固定の文言のエラーを送る
     * @param reply claimReplyForAsyncCompletion()で確保した応答コンテキスト
     * @param errorName D-Busエラー名
     * @param text エラーメッセージ (攻撃者が制御し得る値を含めない固定の文言)
     */
    void sendAsyncError(const CallReply &reply, const QString &errorName, const QString &text);

    /** @brief timerの操作を同時に1件に限るため、実行中の操作があれば拒否する
     * @return 拒否してエラー応答を送出した場合: true
     */
    bool rejectWhileTimerOperationRunning();

    /** @brief timerの有効化または無効化、またはsnapperdへ転送した変更の実行中かを判定する
     * @return 変更を伴うtimerの操作、またはsnapperdへ転送した変更が完了していない場合: true
     * @note CommitRestorePlanが復元の開始を拒否するために使う
     *       timerの操作は変更の直前ごとに復元の実行中でないことを確かめるが、送信済みの変更は取り消せないため、
     *       復元の開始側でも実行中の変更と重ならないようにする
     *       snapperdへ転送した変更も同じ理由で含める (認可前に拒否すれば、復元計画は失敗にならずに残る)
     */
    bool timerChangeInProgress() const;

    /** @brief 認可済みのCommitRestorePlan本体
     * @param manifestId owner束縛されたマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @return 実行開始を受理した場合: true
     */
     bool commitRestorePlanAuthorized(const QString &manifestId, const QString &owner);
    /** @brief 凍結済み計画をサーバ再構築の権威ある比較と照合して検証する
     * @param manifestId owner束縛されたマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @param snapper 検証済み設定のSnapperインスタンス
     * @param source 復元元スナップショットのiterator
     * @param counterpart 比較相手スナップショットのiterator (getSnapshotCurrent()含む)
     * @param snapshotDir pin済みdirfdが指すスナップショット directoryのpath
     * @param pinnedStat pin済みdirfdに対するfstatの結果
     * @return 計画全体が権威ある比較と一致し同一性確認も通った場合true
     */
     bool validateFrozenRestorePlan(const QString &manifestId, const QString &owner, snapper::Snapper *snapper, const snapper::Snapshots::const_iterator &source, const snapper::Snapshots::const_iterator &counterpart, const QString &snapshotDir, const struct stat &pinnedStat);
    /** @brief commit時preflight失敗を計画のfail-closed終端へ落とし込む
     * @param manifestId 検証に失敗したマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @param error registryのmarkFailedへ渡すエラー格納先
     */
     void failRestorePlanPreflight(const QString &manifestId, const QString &owner, qsnapper::restore::ManifestError *error);
    /** @brief 認可が得られなかった復元計画をFailedで終端する
     * @param manifestId 対象マニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     */
     void failRestorePlanAuthorization(const QString &manifestId, const QString &owner);
    /** @brief Snapperインスタンスを取得 (必要に応じて生成 / 再生成)
     * @param configName 設定名
     * @param forceReload 強制再生成フラグ
     */
     snapper::Snapper* getSnapper(const QString &configName = "root", bool forceReload = false);
    /** @brief snapperdへ転送した変更の実行中かを判定する
     * @return 完了の通知を受けていない変更がある場合: true
     */
    bool snapperdOperationRunning() const { return !m_snapperdOperation.isNull(); }

    /** @brief snapperdへ転送した変更の実行中であれば、操作を固定の文言で拒否する
     * @return 拒否してエラー応答を送出した場合: true
     */
    bool rejectWhileSnapperdOperationRunning();

    /** @brief 認可済みのCreateConfig本体
     * @param configName 構文を検証済みの新しい設定名
     * @param subvolume 構文を検証済みのsubvolumeの絶対パス
     * @return 常にfalse (応答はsnapperdの完了後に送る)
     */
    bool createConfigAuthorized(const QString &configName, const QString &subvolume);

    /** @brief 認可済みのDeleteConfig本体
     * @param configName 構文を検証済みの設定名
     * @return 常にfalse (応答はsnapperdの完了後に送る)
     */
    bool deleteConfigAuthorized(const QString &configName);

    /** @brief 認可済みのDeleteSnapshots本体
     * @param configName 検証済みの設定名
     * @param numbers 範囲と件数を検証し、重複を除いた番号
     * @return 常に空 (応答はsnapperdの完了後に送る)
     */
    QList<uint> deleteSnapshotsAuthorized(const QString &configName, const QList<uint> &numbers);

    /** @brief 認可済みのGetFilesystemUsage本体
     * @param configName 検証済みの設定名
     * @return 使用量のCSV
     */
    QString getFilesystemUsageAuthorized(const QString &configName);

    /** @brief 認可済みのListSubvolumes本体
     * @param configName 検証済みの設定名
     * @return サブボリューム一覧のCSV
     */
    QString listSubvolumesAuthorized(const QString &configName);

    /** @brief 指定したconfigに、終わっていない復元計画があるかを判定する
     * @param configName 検証済みの設定名
     * @param uncertain 計画の状態を読めなかった場合にtrueを受け取る (呼び出し側は拒否として扱う)
     * @return 終わっていない計画がある場合: true
     */
    bool hasUnfinishedRestorePlan(const QString &configName, bool *uncertain);

    /** @brief snapperdへ変更を転送し、完了時に結果を読み直して応答する
     * @param reply 非同期に応答するために確保した応答コンテキスト
     * @param mutation snapperdへ転送する変更
     * @param listConfig 事前にsnapperdのスナップショット一覧を確かめるconfig名 (空の場合は確かめない)
     * @param requiredNumbers listConfigの一覧に存在を求める番号
     * @param replyDeadlineMs 呼び出し元へ応答する期限
     * @param verify 完了後に実際の状態を読み直し、応答を送る処理 (期限切れで応答済みの場合はrepliedがtrue)
     */
    void startSnapperdOperation(const CallReply &reply, const QDBusMessage &mutation, const QString &listConfig,
                                const QList<uint> &requiredNumbers, int replyDeadlineMs,
                                std::function<void(qsnapper::dbusservice::SnapperdOperation::Outcome, bool stoppedByRestore,
                                                   const CallReply &reply, bool replied)> verify);

    /** @brief 一覧の取得用にSnapperインスタンスを取得する
     * @param configName 検証済みSnapper設定名
     * @return Snapperインスタンスへのポインタ、失敗時はnullptr
     */
    snapper::Snapper* getSnapperForListing(const QString &configName);

    /** @brief Snapperインスタンスのスナップショット一覧をCSVに整形する
     */
    QString formatSnapshotToCSV(const snapper::Snapper *snapper);

    /** @brief スナップショットタイプ列挙値を文字列に変換する
     */
    QString snapshotTypeToString(int type);

    /** @brief 文字列をスナップショットタイプ列挙値に変換する
     */
    int stringToSnapshotType(const QString &typeStr);

    /** @brief live宛先の親ディレクトリを1回だけ解決し、ディレクトリを作成してmetadataを適用する
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @return ディレクトリと必須metadataを適用できた場合: true
     */
     static bool applyDirectoryBeneathRoot(int sourceDirFd, const QString &sourceRelativePath, const QString &rootPath, const QString &dst);
    /** @brief live宛先の親ディレクトリを1回だけ解決し、通常ファイルをコピーして差し替える
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス (絶対パス・".."/"."成分は不可)
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @param tryReflink FICLONEを先行試行するか
     * @return dataと必須metadataを適用できた場合: true
     */
     static bool copyRegularFileBeneathRoot(int sourceDirFd, const QString &sourceRelativePath, const QString &rootPath, const QString &dst, bool tryReflink);
    /** @brief live宛先の親ディレクトリを1回だけ解決し、symlinkをコピーして差し替える
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @return symlinkを作成できた場合: true
     */
     static bool copySymlinkBeneathRoot(int sourceDirFd, const QString &sourceRelativePath, const QString &rootPath, const QString &dst);
    /** @brief live パスをroot配下で再解決して一時的な兄弟パスへ退避する
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param path 退避対象の絶対パス
     * @param movedPath 実際の退避先
     *                  対象不存在時は空文字列
     * @return 退避または対象不存在時true
     */
     static bool movePathAsideBeneathRoot(const QString &rootPath, const QString &path, QString *movedPath);
    /** @brief 現在のD-Bus呼び出し元unique nameを返す
     * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
     */
    QString callerOwner() const;

    /** @brief マニフェスト操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
     * @param error registryが返したエラー
     * @return 常にfalse
     */
    bool sendManifestError(qsnapper::restore::ManifestError error);

    /** @brief マニフェスト状態をD-Bus contractの小文字表現へ変換する
     * @param state 変換対象状態
     * @return staging/frozen/running/completed/failed/cancelledのいずれか
     */
    static QString restoreManifestStateString(
        qsnapper::restore::ManifestState state);

    /** @brief restorePlanFinished signalに載せる終端状態ごとの固定文言を返す
     * @param state 終端状態
     * @return pathなどの詳細を含まない固定文言
     */
    static QString restorePlanFinishedSignalMessage(
        qsnapper::restore::ManifestState state);

    /** @brief 復元方式をD-Bus contractの文字列表現へ変換する
     * @param mode 変換対象方式
     * @return YaSTまたはダイレクト
     */
    static QString restoreModeString(qsnapper::restore::RestoreMode mode);

    /** @brief RFC4180形式で必要なCSVフィールドをクォートする
     * @param field クォート対象文字列
     * @return CSVへ安全に埋め込めるフィールド
     */
    static QString quoteRestoreStatusCsvField(const QString &field);

    /** @brief executor callbackから単一の凍結済みエントリをlive filesystemへ適用する
     * @param manifestId 実行コンテキストを選択するマニフェストID
     * @param entry 適用対象entry
     * @return entry全体の適用成功時true
     */
     bool applyRestoreEntry(const QString &manifestId, const qsnapper::restore::RestoreEntry &entry);
    /** @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
     * @param manifestId 終端したマニフェストid
     * @param terminal 終端状態
     * @param message 終端理由
     */
     void finishRestorePlan(const QString &manifestId, qsnapper::restore::ManifestState terminal, const QString &message);
    /** @brief 指定マニフェストのmountを可能な限り解除して実行コンテキストを削除する
     * @param manifestId クリーンアップ対象マニフェストID
     */
    void cleanupRestoreExecution(const QString &manifestId);

    /** @brief execution コンテキストに記録されたスナップショットマウントを解除する
     * @param execution マウント元設定とスナップショット番号を持つコンテキスト
     */
    void unmountRestoreExecution(const RestoreExecution &execution);

    /** @brief owner消失時に予約済み実行・マウント・マニフェストを全て破棄する
     * @param owner unregisterされたD-Busユニーク名
     */
    void handleRestoreOwnerUnregistered(const QString &owner);

    /** @brief TTL purgeで消えたactive計画をabandonしマウントおよびクリーンアップする
     */
    void purgeExpiredRestorePlans();

    /** @brief マニフェストを持たないownerをservice watcherから除外する
     */
    void removeUnusedRestoreOwnerWatches();

    /** @brief 実行を開始した復元計画に限り、rootサブボリュームをrwへ戻す安全ネットを実行する
     * @param state commit時に記録した安全ネットの判定材料
     */
    void runRootReadWriteSafetyNet(
        const qsnapper::restore::RootReadWriteSafetyNetState &state);

public:
    /** @brief コンストラクタ
     * @param parent 親QObject
     */
    explicit SnapshotOperations(QObject *parent = nullptr);

    /** @brief デストラクタ
     */
    ~SnapshotOperations();

    /** @brief rootサブボリュームのread-only状態の取得とrw化の処理をテスト用に差し替える
     * @param readOnlyProbe read-only状態を取得する関数 (取得できた場合true)
     * @param readWriteRestorer rwへ戻す関数 (成功した場合true)
     */
     void setRootSubvolumeAccessForTesting(std::function<bool(bool *)> readOnlyProbe, std::function<bool()> readWriteRestorer);

#ifdef QSNAPPER_ENABLE_TEST_HOOKS
    // 以下はQSNAPPER_ENABLE_TEST_HOOKSを定義したテスト用ターゲットだけでコンパイルする
    // 認可を迂回できるため、本番のサービス (src/dbusservice/CMakeLists.txt) では定義しない

    /** @class TestAuthorization
     *  @brief テスト用サービスで、polkitの代わりに使う認可の結果
     */
    enum class TestAuthorization {
        Polkit,     // 本番と同じくpolkitへ問い合わせる
        Grant,      // 即時に許可する (即時許可の経路を通す)
        Defer       // 応答を保留し、resolveTestAuthorizationsで確定する (遅延許可の経路を通す)
    };

    /** @brief 認可の結果をテスト用に固定する
     * @param mode 以降の認可要求に返す結果
     */
    void setAuthorizationForTesting(TestAuthorization mode);

    /** @brief 保留中のテスト用認可の件数を返す
     * @return 保留中の件数
     */
    int pendingTestAuthorizationCount() const;

    /** @brief 保留中のテスト用認可をすべて確定し、各呼び出しの継続を実行する
     * @param granted 許可する場合はtrue
     */
    void resolveTestAuthorizations(bool granted);

    /** @brief 復元の実行中の状態を、mountせずに作る
     *  共通ゲートとgetSnapperの多重防御を、実際の復元を伴わずに検証するために使う
     * @param configName 復元中として記録するconfig名
     */
    void beginRestoreExecutionForTesting(const QString &configName);

    /** @brief beginRestoreExecutionForTestingで作った状態を解除する
     */
    void endRestoreExecutionForTesting();

    /** @brief snapperdへ転送した変更の実行中の状態を、snapperdへ転送せずに作る
     *  configの作成・削除や一括削除の途中に、他の操作と復元の開始が拒否されることを確かめるために使う
     */
    void beginSnapperdOperationForTesting();

    /** @brief beginSnapperdOperationForTestingで作った状態を解除する
     */
    void endSnapperdOperationForTesting();

private:
    TestAuthorization m_testAuthorization = TestAuthorization::Polkit;                                  // 認可の結果
    QList<std::pair<CallReply, std::function<void(const CallReply &, bool)>>> m_testPendingAuthorizations;  // 保留中の認可と継続
#endif

public slots:
    /** @brief 既存のSnapper設定名の一覧を返す
     * @return 設定名の配列
     */
    QStringList ListConfigs();

    /** @brief 指定設定のスナップショット一覧をCSVで返す
     * @param configName 設定名 (空文字列時は"root")
     */
    QString ListSnapshots(const QString &configName);

    /** @brief 新しいスナップショットを作成する
     * @param configName 設定名
     * @param type "single" / "pre" / "post"
     * @param description 説明
     * @param preNumber postタイプ時の対応pre番号
     * @param cleanup クリーンアップアルゴリズム名
     * @param userdata 追加メタデータ
     * @param important 重要フラグ
     */
     QString CreateSnapshot(const QString &configName, const QString &type, const QString &description, int preNumber, const QString &cleanup, const QMap<QString, QString> &userdata, bool important);
    /** @brief 既存スナップショットの属性を更新する
     */
     bool ModifySnapshot(const QString &configName, int number, const QString &description, const QString &cleanup, const QMap<QString, QString> &userdata);
    /** @brief 指定スナップショットを削除する
     */
     bool DeleteSnapshot(const QString &configName, int number);
    /** @brief 指定スナップショットへロールバックする
     */
     bool RollbackSnapshot(const QString &configName, int number);
    /** @brief 現在のシステムと単一スナップショット間の変更ファイル一覧を返す
     */
     QString GetFileChanges(const QString &configName, int snapshotNumber);
    /** @brief 2つのスナップショット間の変更ファイル一覧を返す
     */
     QString GetFileChangesBetween(const QString &configName, int number1, int number2);
    /** @brief 指定ファイルの現在とスナップショット間のunified diff + 詳細を返す
     */
     QString GetFileDiffAndDetails(const QString &configName, int snapshotNumber, const QString &filePath);
    /** @brief 指定ファイルの2スナップショット間のunified diff + 詳細を返す
     */
     QString GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath);
    /** @brief ownerに束縛された空のstaged restore計画を開始する
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号 (0は現在のシステム)
     * @param restoreMode yastまたはdirect
     * @return 成功時manifest id
     */
     QString BeginRestorePlan(const QString &configName, int snapshotNumber, int counterpartSnapshotNumber, const QString &restoreMode);
    /** @brief staging計画へ検証済みentry chunkを原子的に追加する
     * @param manifestId owner束縛されたマニフェストID
     * @param filePaths 復元対象絶対パス列
     * @param changeTypes パスと対応する変更種別列
     * @return chunk全体を追加できた場合: true
     */
     bool StageRestoreEntries(const QString &manifestId, const QStringList &filePaths, const QStringList &changeTypes);
    /** @brief 計画をfreeze後に1度だけ認可し非同期実行を開始する
     * @param manifestId owner束縛されたマニフェストID
     * @return 実行開始を受理した場合: true
     */
    bool CommitRestorePlan(const QString &manifestId);

    /** @brief owner確認済みRunning計画のidle loopを再開する
     * @param manifestId owner束縛されたマニフェストID
     * @return nudgeを受理した場合: true
     */
    bool ContinueRestorePlan(const QString &manifestId);

    /** @brief owner確認済み計画状態をRFC4180 escaping済みCSVで返す
     * @param manifestId owner束縛されたマニフェストID
     * @return ManifestStatusフィールド順のCSV、失敗時空文字列
     */
    QString GetRestorePlanStatus(const QString &manifestId);

    /** @brief owner確認済み非終端計画へ境界cancellationを要求する
     * @param manifestId owner束縛されたマニフェストID
     * @return cancellationを受理した場合: true
     */
    bool CancelRestorePlan(const QString &manifestId);

    /** @brief Snapperが1つ以上設定されているかを返す
     */
    bool IsConfigured();

    /** @brief Snapper設定に値を書き込む
     */
     bool WriteSnapperConfig(const QString &configName, const QMap<QString, QString> &settings);
    /** @brief Snapperクォータ機能をセットアップする
     */
    bool SetupQuota(const QString &configName);

    /** @brief Snapper設定の値を読み取る (Settings画面の表示に使う)
     * @param configName 設定名 (空文字列は"root"として扱う)
     * @return 編集できる18キーとSUBVOLUME / FSTYPE / QGROUPの値
     */
    QMap<QString, QString> GetConfig(const QString &configName);

    /** @brief snapperのsystemd timerの状態を読み取る
     * @return unit名から (UnitFileState, ActiveState) へのマップ
     */
    SnapperTimerStateMap GetTimerStates();

    /** @brief Btrfsのsubvolumeに新しいSnapper設定を作成する (snapperdへ転送する)
     * @param configName 新しい設定名 (英数字で始まる英数字・下線・ハイフンの1から64文字)
     * @param subvolume subvolumeの最上位ディレクトリの絶対パス (「/」は扱わない)
     * @return 作成され、読み直した設定が要求どおりの場合: true
     */
    bool CreateConfig(const QString &configName, const QString &subvolume);

    /** @brief Snapper設定と、その全てのスナップショットを削除する (snapperdへ転送する)
     * @param configName 削除する設定名 (rootの設定は扱わない)
     * @return 削除され、読み直した設定の一覧に無い場合: true
     */
    bool DeleteConfig(const QString &configName);

    /** @brief 複数のスナップショットを1回の認可で削除する (snapperdへ転送する)
     * @param configName 設定名 (空文字列は"root"として扱う)
     * @param numbers 削除する番号 (1以上、重複は除き、件数に上限がある)
     * @return 読み直した一覧で削除を確かめた番号 (要求より少ない場合は一部だけ削除された)
     */
    QList<uint> DeleteSnapshots(const QString &configName, const QList<uint> &numbers);

    /** @brief configのsubvolumeがあるBtrfsの割当量と使用量を読み取る
     * @param configName 設定名 (空文字列は"root"として扱う)
     * @return 使用量のCSV
     */
    QString GetFilesystemUsage(const QString &configName);

    /** @brief configのsubvolumeがあるBtrfsのサブボリュームの一覧を読み取る
     * @param configName 設定名 (空文字列は"root"として扱う)
     * @return サブボリューム一覧のCSV (件数・サイズ・時間の上限で打ち切った場合は先頭の行で示す)
     */
    QString ListSubvolumes(const QString &configName);

    /** @brief snapperのsystemd timerを有効化して起動する、または停止して無効化する
     * @param unit 許可リストにあるunit名
     * @param enabled trueで有効化と起動、falseで停止と無効化
     * @return 読み直した状態が要求どおりになった場合: true
     */
    bool SetTimerEnabled(const QString &unit, bool enabled);

signals:
    /** @brief staged restore計画の進捗通知
     * @param manifestId 実行中計画ID
     * @param current 完了エントリ数
     * @param total 凍結時の総エントリ数
     */
     void restorePlanProgress(const QString &manifestId, int current, int total);
    /** @brief staged restore計画の終端通知
     * @param manifestId 終端した計画ID
     * @param terminalState completed / failed / cancelledのいずれか
     * @param message 終端状態ごとの固定文言
     */
     void restorePlanFinished(const QString &manifestId, const QString &terminalState, const QString &message);
};

#endif // SNAPSHOTOPERATIONS_H
