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
    void replyError(QDBusError::ErrorType type, const QString &text);

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
     */
    template <typename T, typename Body>
    void completeDeferredCall(const CallReply &reply, const Body &body, bool granted)
    {
        m_deferredReply = reply;
        T result{};
        if (granted) {
            result = body();
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
     *  @tparam T 対象D-Busメソッドの戻り値型
     * @param actionId PolkitアクションID
     * @param body 認可済みの本体処理
     * @return 同期実行時は本体の戻り値、それ以外はT{}
     */
    template <typename T, typename Body>
    T authorizeThen(const QString &actionId, Body body)
    {
        const AuthorizationOutcome outcome = beginAuthorization(
            actionId,
            [this, body](const CallReply &reply, bool granted) {
                completeDeferredCall<T>(reply, body, granted);
            });

        if (outcome == AuthorizationOutcome::Granted) {
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
