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

namespace snapper {
    class Snapper;
    class Comparison;
}

namespace qsnapper::diff {

    /**
     * @brief unified diff生成の資源上限
     *
     * rootで動作する単一スレッドのサービスが、巨大なファイルや差分過多の入力でメモリやCPUを使い果たさないよう制限する
     */
    struct UnifiedDiffLimits {
        qint64 maxFileBytes = 4LL * 1024 * 1024;        // 1ファイルあたりの最大読み込みサイズ
        qint64 binaryProbeBytes = 8LL * 1024;           // バイナリ判定 (NULの有無) を行う先頭のバイト数
        qint64 maxLinesPerFile = 262144;                // 1ファイルあたりの最大行数
        qint64 maxTraceBytes = 64LL * 1024 * 1024;      // Myers diffの作業領域 (V配列とtrace) の最大サイズ
        qint64 maxSteps = 64LL * 1024 * 1024;           // Myers diffの探索ステップ数の上限 (CPU時間の上限)
    };

    /**
     * @brief unified diffの生成結果
     */
    struct UnifiedDiffResult {
        QString text;           // unified diff (差分なし・省略時は空)
        QString omittedReason;  // 省略理由 (binary / too_large / too_many_changes / special_file)、省略していなければ空
    };

    /**
     * @brief 2つのファイルを読み込み、unified diff形式の文字列を生成する
     *
     * "diff -u"コマンドと互換性のあるフォーマットで、QMLのformatDiffHtml()でパース可能
     * FIFOやデバイスは開かず、上限を超える入力やバイナリは差分を生成せずに省略理由を返す
     *
     * @param oldPath 旧ファイルパス (--- ヘッダに使用)
     * @param newPath 新ファイルパス (+++ ヘッダに使用)
     * @param limits 資源上限
     * @return 生成結果 (読み込めないファイルは従来どおり差分なし・省略理由なし)
     */
    UnifiedDiffResult generateUnifiedDiff(const QString &oldPath,
                                          const QString &newPath,
                                          const UnifiedDiffLimits &limits = UnifiedDiffLimits());

}

namespace qsnapper::restore {

    /**
     * @brief pin済みの復元元ディレクトリが、ファイルシステムの側で読み取り専用であるかを判定する
     *
     * btrfsでは、pin済みfdが属するsubvolumeのread-onlyフラグを見る
     * それ以外 (LVM thinのxfs / ext4など) では、snapperがsnapshotをMS_RDONLYでmountするため、mountが読み取り専用であることを見る
     * 判定できない場合は読み取り専用とみなさない
     *
     * @param snapshotDirFd pin済みの復元元ディレクトリのfd
     * @param fstype configのFSTYPE
     * @return 読み取り専用であることを確認できた場合はtrue
     */
    bool isPinnedRestoreSourceReadOnly(int snapshotDirFd, const std::string &fstype);

}

namespace qsnapper::security {

    /**
     * @brief polkitプロンプト待ちの件数を、呼び出し元UID単位と全体の両方で数える
     *
     * プロンプトはタイムアウトを持たないため、未応答のまま滞留し得る
     * 全体の上限だけでは、1人のローカルユーザが上限まで積むことで他のユーザの操作をすべて拒否させられる
     * UID単位の上限で1ユーザが占有できる量を抑え、全体の上限で総量を抑える
     */
    class PendingAuthorizationBudget
    {
    public:
        /**
         * @brief 上限を指定して構築する
         * @param perUidLimit 1つのUIDが同時に保持できる件数の上限
         * @param globalLimit 全体で同時に保持できる件数の上限
         */
        PendingAuthorizationBudget(int perUidLimit, int globalLimit);

        /**
         * @brief 1件分の枠を確保する
         * @param uid 呼び出し元のUID
         * @return UID単位と全体の両方の上限内であれば確保してtrue、それ以外は何もせずfalse
         */
        bool tryAcquire(uint uid);

        /**
         * @brief tryAcquire()で確保した1件分の枠を返す
         * @param uid tryAcquire()に渡したUID
         */
        void release(uint uid);

        /**
         * @brief 全体の保持件数を返す
         * @return 保持件数
         */
        int total() const;

        /**
         * @brief 指定したUIDの保持件数を返す
         * @param uid 呼び出し元のUID
         * @return 保持件数
         */
        int countFor(uint uid) const;

    private:
        int m_perUidLimit;          // UID単位の上限
        int m_globalLimit;          // 全体の上限
        int m_total = 0;            // 全体の保持件数
        QMap<uint, int> m_perUid;   // UIDごとの保持件数 (0件になったUIDは削除する)
    };

}

class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    /**
     * @brief 非同期復元計画がchunk間で必要とする固定実行コンテキスト
     */
    struct RestoreExecution {
        QString owner;
        QString configName;
        int snapshotNumber = -1;
        QString snapshotDir;
        // 認可時に開いて計画寿命の間保持するスナップショット dirfd
        // ソース読み取りを全て本fd相対で行うことで、実行中のスナップショット削除 / 同番号スナップショットの再作成 / mount状態の変化に依存せず、
        // 認可された計画が参照したスナップショットそのものから復元し続ける
        int snapshotDirFd = -1;
        // commit時に固定したconfigのSUBVOLUME (正規化前)
        // 復元先は"<SUBVOLUME>/<config相対名>"であり、"/"基準で解決してはならない
        QString subvolume;
        // 復元後にrootサブボリュームをrwへ戻す安全ネットの実行条件 (commit時に記録する)
        qsnapper::restore::RootReadWriteSafetyNetState rootReadWriteState;
        bool useReflink = false;
        bool removeOnTypechanged = false;
        bool mounted = false;
    };

    /**
     * @brief 認可待ちを跨いで応答するためにキャプチャしたD-Bus呼び出しコンテキスト
     *
     * QDBusContext::message() / sendErrorReply() はスロットから戻った後は使用できない
     * polkitプロンプトを待って遅延応答する経路では、スロット冒頭でキャプチャした本構造体の値を用いて返信する
     */
    struct CallReply {
        QDBusMessage message;                                       // 返信先のキャプチャ済みメッセージ
        QDBusConnection connection = QDBusConnection::systemBus();  // 応答送出に用いる接続
        bool fromDBus = false;                                      // D-Bus経由の呼び出しか
        bool replied = false;                                       // error / valueを送出済みか
    };

    /**
     * @brief 認可要求の即時結果
     */
    enum class AuthorizationOutcome {
        Granted,    // 対話なしで許可済み
                    // 呼び出し元はそのまま同期実行してよい
        Denied,     // 拒否 (D-Busエラー応答は送出済み)
        Deferred    // polkitプロンプト待ち
                    // 応答は完了継続から送出される
    };

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
    qsnapper::restore::RestoreManifestRegistry m_restoreRegistry;
    qsnapper::restore::RestorePlanExecutor m_restoreExecutor;
    QDBusServiceWatcher *m_ownerWatcher = nullptr;
    QMap<QString, RestoreExecution> m_restoreExecutions;
    QMap<QString, QString> m_restorePlanOwners;
    qsnapper::restore::FinishedRestorePlanStore m_finishedRestorePlans; // 終端した計画の詳細 (ownerだけがGetRestorePlanStatusで取得する)
    std::function<bool(bool *)> m_rootReadOnlyProbe;    // rootサブボリュームのread-only状態を取得する (テストで差し替え可能)
    std::function<bool()> m_rootReadWriteRestorer;      // rootサブボリュームをrwへ戻す (テストで差し替え可能)

    std::optional<CallReply> m_deferredReply;               // 遅延応答の継続を実行している間のみ有効
                                                            // replyError()がsendErrorReply()ではなくキャプチャ済みmessageを使う判断に用いる
    qsnapper::security::PendingAuthorizationBudget m_pendingAuthorizations{
        MaxPendingAuthorizationsPerUid, MaxPendingAuthorizations};  // polkitプロンプト待ちの件数 (呼び出し元UID単位と全体)

private:
    /**
     * @brief アイドルタイマをリセットする
     *
     * D-Busメソッドの先頭で呼び出し、無操作5分による自動終了を先送りする
     * 認可待ちが1件でもある間はタイマを止めたままにする (プロンプト応答を待つ間にサービスが自動終了すると、ユーザが認証した直後に呼び出しが失われるため)
     */
    void resetIdleTimer();

    /**
     * @brief 現在のD-Bus呼び出しの応答コンテキストをキャプチャする
     *
     * message()はスロットから戻ると無効になるため、認可待ちを跨ぐ経路では本関数の戻り値を保持して応答する
     *
     * @return キャプチャ済み応答コンテキスト (D-Bus経由でなければfromDBus=false)
     */
    CallReply captureCallReply() const;

    /**
     * @brief 同期応答と遅延応答のどちらでも正しくD-Busエラーを返す
     *
     * 継続実行中 (m_deferredReplyが有効) はキャプチャ済みmessageから
     * createErrorReply()して送出し、それ以外はQDBusContext::sendErrorReply()に委ねる
     *
     * @param type 返すD-Busエラー種別
     * @param text エラーメッセージ
     */
    void replyError(QDBusError::ErrorType type, const QString &text);

    /**
     * @brief polkit認可を要求する (対話が必要な場合のみ非同期化する)
     *
     * 高速経路として対話を許可しない問い合わせを先に行う
     * allow_active=yesやauth_admin_keepのキャッシュ済み認可はここでGrantedとなり、プロンプトが出ないためイベントループはミリ秒しか止まらない
     * 対話が必要 (challenge) な場合のみ非同期APIへ回し、setDelayedReply(true)を立てた上でDeferredを返す
     * 同期版はタイムアウトを持たず、未応答プロンプト1つでサービス全体のイベントループが無期限に凍結するため、対話経路では使わない
     *
     * @param actionId PolkitアクションID
     * @param continuation 認可完了時に呼ぶ継続 (キャプチャ済み応答コンテキストと可否を受け取る)
     * @return 即時許可 / 即時拒否 / 遅延のいずれか
     */
    AuthorizationOutcome beginAuthorization(const QString &actionId, std::function<void(const CallReply &, bool)> continuation);

    /**
     * @brief 認可待ち1件の終了を記録し、必要ならアイドルタイマを再開する
     * @param callerUid 認可待ちを開始したときの呼び出し元UID
     */
    void endPendingAuthorization(uint callerUid);

    /**
     * @brief 遅延応答経路で本体を実行し、戻り値をD-Bus応答として送出する
     *
     * 本体が既にreplyError()でエラーを返している場合は値応答を送らない
     *
     * @tparam T 対象D-Busメソッドの戻り値型
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

    /**
     * @brief 認可してから本体を実行する共通ゲート
     *
     * 対話不要ならその場で本体を同期実行して戻り値を返す (従来と同じ挙動)
     * 対話が必要な場合は遅延応答へ切り替え、既定値を返して呼び出しを終える
     *
     * @tparam T 対象D-Busメソッドの戻り値型
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

    /**
     * @brief configNameを正規化＋検証し、不正ならD-Busエラー応答を返す
     *
     * 空文字列の入力は"root"に正規化した上で、qsnapper::security::validateConfigNameで検証する
     * これにより呼び出し側の"空ならroot"デフォルト割当パターンが不要になり、空入力に対する一貫した扱い (常に"root"として受理) を保証する
     *
     * 各D-Busスロットの先頭 (beginAuthorization より前) で呼び出すこと
     * Polkitプロンプトが出てから"invalid config name"で蹴られるUXを避けるため順序が重要
     *
     * @param configName 検査対象の設定名 (空文字列は"root"として扱う)
     * @return 正規化後の設定名 (有効な場合) で、無効でエラー応答済みならstd::nullopt
     */
    std::optional<QString> resolveConfigOrFail(const QString &configName);

    /**
     * @brief 認可済みのListConfigs本体
     * @return 設定名の配列
     */
    QStringList listConfigsAuthorized();

    /**
     * @brief 認可済みのListSnapshots本体
     * @param configName 検証済み設定名
     */
    QString listSnapshotsAuthorized(const QString &configName);

    /**
     * @brief 認可済みのCreateSnapshot本体
     * @param configName 検証済み設定名
     */
    QString createSnapshotAuthorized(const QString &configName,
                                     const QString &type,
                                     const QString &description,
                                     int preNumber,
                                     const QString &cleanup,
                                     const QMap<QString, QString> &userdata,
                                     bool important);

    /**
     * @brief 認可済みのModifySnapshot本体
     * @param configName 検証済み設定名
     */
    bool modifySnapshotAuthorized(const QString &configName,
                                  int number,
                                  const QString &description,
                                  const QString &cleanup,
                                  const QMap<QString, QString> &userdata);

    /**
     * @brief 認可済みのDeleteSnapshot本体
     * @param configName 検証済み設定名
     */
    bool deleteSnapshotAuthorized(const QString &configName, int number);

    /**
     * @brief 認可済みのRollbackSnapshot本体
     * @param configName 検証済み設定名
     */
    bool rollbackSnapshotAuthorized(const QString &configName, int number);

    /**
     * @brief 認可済みのGetFileChanges本体
     * @param configName 検証済み設定名
     */
    QString getFileChangesAuthorized(const QString &configName,
                                     int snapshotNumber);

    /**
     * @brief 認可済みのGetFileChangesBetween本体
     * @param configName 検証済み設定名
     */
    QString getFileChangesBetweenAuthorized(const QString &configName,
                                            int number1,
                                            int number2);

    /**
     * @brief 認可済みのGetFileDiffAndDetails本体
     * @param configName 検証済み設定名
     * @param filePath 検証済み絶対パス
     */
    QString getFileDiffAndDetailsAuthorized(const QString &configName,
                                            int snapshotNumber,
                                            const QString &filePath);

    /**
     * @brief 認可済みのGetFileDiffBetween本体
     * @param configName 検証済み設定名
     * @param filePath 検証済み絶対パス
     */
    QString getFileDiffBetweenAuthorized(const QString &configName,
                                         int number1,
                                         int number2,
                                         const QString &filePath);

    /**
     * @brief 認可済みのWriteSnapperConfig本体
     * @param configName 検証済み設定名
     */
    bool writeSnapperConfigAuthorized(const QString &configName,
                                      const QMap<QString, QString> &settings);

    /**
     * @brief 認可済みのSetupQuota本体
     * @param configName 検証済み設定名
     */
    bool setupQuotaAuthorized(const QString &configName);

    /**
     * @brief 認可済みのCommitRestorePlan本体
     *
     * 認可待ちの間にcancel / TTL失効 / owner消失 / 他計画の実行開始が起こり得るため、
     * mountやexecutor起動の前に状態を再検証する
     * さらに凍結済みの全entryが、サーバが再構築した権威ある比較
     * (source, counterpart) と一致することをcommit時に確認する
     *
     * @param manifestId owner束縛されたマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @return 実行開始を受理した場合: true
     */
    bool commitRestorePlanAuthorized(const QString &manifestId,
                                     const QString &owner);

    /**
     * @brief 凍結済み計画をサーバ再構築の権威ある比較と照合して検証する
     *
     * ローカルの非キャッシュComparison (mount=false) を1つだけ構築し、
     * 全entryの検証とpin済みdirfdの同一性確認 (st_dev / st_ino) を行ってから破棄する
     *
     * キャッシュを使わない理由は、Policy::Reuseが認可前生成のComparisonを返し得ること、Policy::Refreshがmount済みオブジェクトをキャッシュに残すことにある
     * (libsnapperのmount_use_countはboolではないため、キャッシュに残ったmountはユーザの明示unmountを飛ばす)
     *
     * 本関数は認可を一切行わない
     *
     * @param manifestId owner束縛されたマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @param snapper 検証済み設定のSnapperインスタンス
     * @param source 復元元スナップショットのiterator
     * @param counterpart 比較相手スナップショットのiterator (getSnapshotCurrent()含む)
     * @param snapshotDir pin済みdirfdが指すスナップショット directoryのpath
     * @param pinnedStat pin済みdirfdに対するfstatの結果
     * @return 計画全体が権威ある比較と一致し同一性確認も通った場合true
     */
    bool validateFrozenRestorePlan(const QString &manifestId,
                                   const QString &owner,
                                   snapper::Snapper *snapper,
                                   const snapper::Snapshots::const_iterator &source,
                                   const snapper::Snapshots::const_iterator &counterpart,
                                   const QString &snapshotDir,
                                   const struct stat &pinnedStat);

    /**
     * @brief commit時preflight失敗を計画のfail-closed終端へ落とし込む
     *
     * mount / dirfd確保は準備であって復元の変異ではないため、executor起動前に失敗していればlive filesystemは一切変化しない
     * cleanup -> markFailed -> 汎用warning -> replyErrorの順で終端する
     *
     * エラー文とlogに攻撃者が制御できるpathを含めてはならない
     *
     * @param manifestId 検証に失敗したマニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     * @param error registryのmarkFailedへ渡すエラー格納先
     */
    void failRestorePlanPreflight(const QString &manifestId,
                                  const QString &owner,
                                  qsnapper::restore::ManifestError *error);

    /**
     * @brief 認可が得られなかった復元計画をFailedで終端する
     * @param manifestId 対象マニフェストID
     * @param owner 認可前にキャプチャした呼び出し元unique name
     */
    void failRestorePlanAuthorization(const QString &manifestId,
                                      const QString &owner);

    /**
     * @brief Snapperインスタンスを取得 (必要に応じて生成 / 再生成)
     * @param configName 設定名
     * @param forceReload 強制再生成フラグ
     */
    snapper::Snapper* getSnapper(const QString &configName = "root",
                                 bool forceReload = false);

    /**
     * @brief 一覧の取得用にSnapperインスタンスを取得する
     *
     * 同じ設定のインスタンスがあり、.snapshots の指紋が読み込み時と一致する場合だけ、再生成せずに再利用する
     * 指紋が一致しない・計算できない・未記録の場合は再生成し、外部 (snapper CLIやtimer) による作成・削除・変更を反映する
     *
     * @param configName 検証済みSnapper設定名
     * @return Snapperインスタンスへのポインタ、失敗時はnullptr
     */
    snapper::Snapper* getSnapperForListing(const QString &configName);

    /**
     * @brief Snapperインスタンスのスナップショット一覧をCSVに整形する
     */
    QString formatSnapshotToCSV(const snapper::Snapper *snapper);

    /**
     * @brief スナップショットタイプ列挙値を文字列に変換する
     */
    QString snapshotTypeToString(int type);

    /**
     * @brief 文字列をスナップショットタイプ列挙値に変換する
     */
    int stringToSnapshotType(const QString &typeStr);

    /**
     * @brief live宛先の親ディレクトリを1回だけ解決し、ディレクトリを作成してmetadataを適用する
     *
     * 作成とopenを同じ親dirfd上で行い、所有者・mode・ACL (access / default)・xattrをfd経由でsnapshot側に合わせる
     *
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @return ディレクトリと必須metadataを適用できた場合: true
     */
    static bool applyDirectoryBeneathRoot(int sourceDirFd,
                                          const QString &sourceRelativePath,
                                          const QString &rootPath,
                                          const QString &dst);

    /**
     * @brief live宛先の親ディレクトリを1回だけ解決し、通常ファイルをコピーして差し替える
     *
     * 一時ファイルの作成からrenameat()まで同じ親dirfdを使い、名前を再解決しない
     * ソースはpin済みスナップショット dirfd相対で解決する
     * 実行中にsnapshotDirのパス上で削除 / 再作成 / 差し替えが起きても、fdが指すinodeを読み続けるため、
     * 認可時と異なる内容を読み込むことがない
     *
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス (絶対パス・".."/"."成分は不可)
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @param tryReflink FICLONEを先行試行するか
     * @return dataと必須metadataを適用できた場合: true
     */
    static bool copyRegularFileBeneathRoot(int sourceDirFd,
                                           const QString &sourceRelativePath,
                                           const QString &rootPath,
                                           const QString &dst,
                                           bool tryReflink);

    /**
     * @brief live宛先の親ディレクトリを1回だけ解決し、symlinkをコピーして差し替える
     *
     * ソースはpin済みスナップショット dirfd相対で解決する (copyRegularFileBeneathRootと同じ理由)
     *
     * @param sourceDirFd pin済みのスナップショット dirfd
     * @param sourceRelativePath snapshotDirからの相対ソースパス
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param dst live filesystem上の絶対パス
     * @return symlinkを作成できた場合: true
     */
    static bool copySymlinkBeneathRoot(int sourceDirFd,
                                       const QString &sourceRelativePath,
                                       const QString &rootPath,
                                       const QString &dst);

    /**
     * @brief live パスをroot配下で再解決して一時的な兄弟パスへ退避する
     * @param rootPath 名前解決の基準とする絶対パス (configのSUBVOLUME)
     * @param path 退避対象の絶対パス
     * @param movedPath 実際の退避先
     *                  対象不存在時は空文字列
     * @return 退避または対象不存在時true
     */
    static bool movePathAsideBeneathRoot(const QString &rootPath,
                                         const QString &path,
                                         QString *movedPath);

    /**
     * @brief 現在のD-Bus呼び出し元unique nameを返す
     * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
     */
    QString callerOwner() const;

    /**
     * @brief マニフェスト操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
     * @param error registryが返したエラー
     * @return 常にfalse
     */
    bool sendManifestError(qsnapper::restore::ManifestError error);

    /**
     * @brief マニフェスト状態をD-Bus contractの小文字表現へ変換する
     * @param state 変換対象状態
     * @return staging/frozen/running/completed/failed/cancelledのいずれか
     */
    static QString restoreManifestStateString(
        qsnapper::restore::ManifestState state);

    /**
     * @brief restorePlanFinished signalに載せる終端状態ごとの固定文言を返す
     * @param state 終端状態
     * @return pathなどの詳細を含まない固定文言
     */
    static QString restorePlanFinishedSignalMessage(
        qsnapper::restore::ManifestState state);

    /**
     * @brief 復元方式をD-Bus contractの文字列表現へ変換する
     * @param mode 変換対象方式
     * @return YaSTまたはダイレクト
     */
    static QString restoreModeString(qsnapper::restore::RestoreMode mode);

    /**
     * @brief RFC4180形式で必要なCSVフィールドをクォートする
     *
     * カンマまたはダブルクォートを含むフィールドはダブルクォートで囲み、内部のダブルクォートを2重化する
     *
     * @param field クォート対象文字列
     * @return CSVへ安全に埋め込めるフィールド
     */
    static QString quoteRestoreStatusCsvField(const QString &field);

    /**
     * @brief executor callbackから単一の凍結済みエントリをlive filesystemへ適用する
     * @param manifestId 実行コンテキストを選択するマニフェストID
     * @param entry 適用対象entry
     * @return entry全体の適用成功時true
     */
    bool applyRestoreEntry(const QString &manifestId,
                           const qsnapper::restore::RestoreEntry &entry);

    /**
     * @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
     * @param manifestId 終端したマニフェストid
     * @param terminal 終端状態
     * @param message 終端理由
     */
    void finishRestorePlan(const QString &manifestId,
                           qsnapper::restore::ManifestState terminal,
                           const QString &message);

    /**
     * @brief 指定マニフェストのmountを可能な限り解除して実行コンテキストを削除する
     * @param manifestId クリーンアップ対象マニフェストID
     */
    void cleanupRestoreExecution(const QString &manifestId);

    /**
     * @brief execution コンテキストに記録されたスナップショットマウントを解除する
     * @param execution マウント元設定とスナップショット番号を持つコンテキスト
     */
    void unmountRestoreExecution(const RestoreExecution &execution);

    /**
     * @brief owner消失時に予約済み実行・マウント・マニフェストを全て破棄する
     * @param owner unregisterされたD-Busユニーク名
     */
    void handleRestoreOwnerUnregistered(const QString &owner);

    /**
     * @brief TTL purgeで消えたactive計画をabandonしマウントおよびクリーンアップする
     */
    void purgeExpiredRestorePlans();

    /**
     * @brief マニフェストを持たないownerをservice watcherから除外する
     */
    void removeUnusedRestoreOwnerWatches();

    /**
     * @brief 実行を開始した復元計画に限り、rootサブボリュームをrwへ戻す安全ネットを実行する
     *
     * 認可済みで実行を開始し、対象設定のSUBVOLUMEが "/" で、復元前の状態がrwだった場合だけ、復元後にread-onlyになっていればrwへ戻す
     * 条件を1つでも満たさない場合は、read-only状態の取得も含めて特権操作を一切行わない
     *
     * @param state commit時に記録した安全ネットの判定材料
     */
    void runRootReadWriteSafetyNet(
        const qsnapper::restore::RootReadWriteSafetyNetState &state);

public:
    /**
     * @brief コンストラクタ
     * @param parent 親QObject
     */
    explicit SnapshotOperations(QObject *parent = nullptr);

    /**
     * @brief デストラクタ
     */
    ~SnapshotOperations();

    /**
     * @brief rootサブボリュームのread-only状態の取得とrw化の処理をテスト用に差し替える
     *
     * D-Busへは公開しない (slotではない)
     *
     * @param readOnlyProbe read-only状態を取得する関数 (取得できた場合true)
     * @param readWriteRestorer rwへ戻す関数 (成功した場合true)
     */
    void setRootSubvolumeAccessForTesting(std::function<bool(bool *)> readOnlyProbe,
                                          std::function<bool()> readWriteRestorer);

public slots:
    /**
     * @brief 既存のSnapper設定名の一覧を返す
     * @return 設定名の配列
     */
    QStringList ListConfigs();

    /**
     * @brief 指定設定のスナップショット一覧をCSVで返す
     * @param configName 設定名 (空文字列時は"root")
     */
    QString ListSnapshots(const QString &configName);

    /**
     * @brief 新しいスナップショットを作成する
     * @param configName 設定名
     * @param type "single" / "pre" / "post"
     * @param description 説明
     * @param preNumber postタイプ時の対応pre番号
     * @param cleanup クリーンアップアルゴリズム名
     * @param userdata 追加メタデータ
     * @param important 重要フラグ
     */
    QString CreateSnapshot(const QString &configName,
                           const QString &type,
                           const QString &description,
                           int preNumber,
                           const QString &cleanup,
                           const QMap<QString, QString> &userdata,
                           bool important);

    /**
     * @brief 既存スナップショットの属性を更新する
     */
    bool ModifySnapshot(const QString &configName,
                        int number,
                        const QString &description,
                        const QString &cleanup,
                        const QMap<QString, QString> &userdata);

    /**
     * @brief 指定スナップショットを削除する
     */
    bool DeleteSnapshot(const QString &configName,
                        int number);

    /**
     * @brief 指定スナップショットへロールバックする
     */
    bool RollbackSnapshot(const QString &configName,
                          int number);

    /**
     * @brief 現在のシステムと単一スナップショット間の変更ファイル一覧を返す
     */
    QString GetFileChanges(const QString &configName,
                           int snapshotNumber);

    /**
     * @brief 2つのスナップショット間の変更ファイル一覧を返す
     */
    QString GetFileChangesBetween(const QString &configName,
                                  int number1,
                                  int number2);

    /**
     * @brief 指定ファイルの現在とスナップショット間のunified diff + 詳細を返す
     */
    QString GetFileDiffAndDetails(const QString &configName,
                                  int snapshotNumber,
                                  const QString &filePath);

    /**
     * @brief 指定ファイルの2スナップショット間のunified diff + 詳細を返す
     */
    QString GetFileDiffBetween(const QString &configName,
                               int number1,
                               int number2,
                               const QString &filePath);

    /**
     * @brief ownerに束縛された空のstaged restore計画を開始する
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号 (0は現在のシステム)
     * @param restoreMode yastまたはdirect
     * @return 成功時manifest id
     */
    QString BeginRestorePlan(const QString &configName,
                             int snapshotNumber,
                             int counterpartSnapshotNumber,
                             const QString &restoreMode);

    /**
     * @brief staging計画へ検証済みentry chunkを原子的に追加する
     * @param manifestId owner束縛されたマニフェストID
     * @param filePaths 復元対象絶対パス列
     * @param changeTypes パスと対応する変更種別列
     * @return chunk全体を追加できた場合: true
     */
    bool StageRestoreEntries(const QString &manifestId,
                             const QStringList &filePaths,
                             const QStringList &changeTypes);

    /**
     * @brief 計画をfreeze後に1度だけ認可し非同期実行を開始する
     * @param manifestId owner束縛されたマニフェストID
     * @return 実行開始を受理した場合: true
     */
    bool CommitRestorePlan(const QString &manifestId);

    /**
     * @brief owner確認済みRunning計画のidle loopを再開する
     * @param manifestId owner束縛されたマニフェストID
     * @return nudgeを受理した場合: true
     */
    bool ContinueRestorePlan(const QString &manifestId);

    /**
     * @brief owner確認済み計画状態をRFC4180 escaping済みCSVで返す
     * @param manifestId owner束縛されたマニフェストID
     * @return ManifestStatusフィールド順のCSV、失敗時空文字列
     */
    QString GetRestorePlanStatus(const QString &manifestId);

    /**
     * @brief owner確認済み非終端計画へ境界cancellationを要求する
     * @param manifestId owner束縛されたマニフェストID
     * @return cancellationを受理した場合: true
     */
    bool CancelRestorePlan(const QString &manifestId);

    /**
     * @brief Snapperが1つ以上設定されているかを返す
     */
    bool IsConfigured();

    /**
     * @brief Snapper設定に値を書き込む
     */
    bool WriteSnapperConfig(const QString &configName,
                            const QMap<QString, QString> &settings);

    /**
     * @brief Snapperクォータ機能をセットアップする
     */
    bool SetupQuota(const QString &configName);


signals:
    /**
     * @brief staged restore計画の進捗通知
     * @param manifestId 実行中計画ID
     * @param current 完了エントリ数
     * @param total 凍結時の総エントリ数
     *
     * signalはsystem busの全接続へ届くため、復元中のpathは載せない
     */
    void restorePlanProgress(const QString &manifestId,
                             int current,
                             int total);

    /**
     * @brief staged restore計画の終端通知
     * @param manifestId 終端した計画ID
     * @param terminalState completed / failed / cancelledのいずれか
     * @param message 終端状態ごとの固定文言
     *
     * signalはsystem busの全接続へ届くため、失敗したpathなどの詳細は載せない
     * 詳細は計画のownerがGetRestorePlanStatusで取得する (終端後も短時間保持する)
     */
    void restorePlanFinished(const QString &manifestId,
                             const QString &terminalState,
                             const QString &message);
};

#endif // SNAPSHOTOPERATIONS_H
