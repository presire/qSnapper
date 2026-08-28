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
#include <functional>
#include <memory>
#include <optional>
#include "comparisoncache.h"
#include "restoremanifest.h"
#include "restoreplanexecutor.h"

namespace snapper {
    class Snapper;
    class Comparison;
}

class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    /**
     * @brief 非同期復元計画がchunk間で必要とする固定実行context
     */
    struct RestoreExecution {
        QString owner;
        QString configName;
        int snapshotNumber = -1;
        QString snapshotDir;
        // 認可時に開いて計画寿命の間保持するsnapshot dirfd。
        // ソース読み取りを全て本fd相対で行うことで、実行中のsnapshot削除 /
        // 同番号snapshotの再作成 / mount状態の変化に依存せず、
        // 認可された計画が参照したsnapshotそのものから復元し続ける
        int snapshotDirFd = -1;
        bool useReflink = false;
        bool removeOnTypechanged = false;
        bool mounted = false;
    };

    /**
     * @brief 認可待ちを跨いで応答するためにcaptureしたD-Bus呼び出しcontext
     *
     * QDBusContext::message() / sendErrorReply() はスロットから戻った後は使えない。
     * polkitプロンプトを待って遅延応答する経路では、スロット冒頭でcaptureした
     * 本構造体の値を用いて返信する
     */
    struct CallReply {
        QDBusMessage message;                                       // 返信先のcapture済みmessage
        QDBusConnection connection = QDBusConnection::systemBus();  // 応答送出に用いる接続
        bool fromDBus = false;                                      // D-Bus経由の呼び出しか
        bool replied = false;                                       // error/valueを送出済みか
    };

    /**
     * @brief 認可要求の即時結果
     */
    enum class AuthorizationOutcome {
        Granted,    // 対話なしで許可済み。呼び出し元はそのまま同期実行してよい
        Denied,     // 拒否 (D-Busエラー応答は送出済み)
        Deferred    // polkitプロンプト待ち。応答は完了継続から送出される
    };

    /**
     * @brief アイドルタイムアウト (5分)
     *
     * 最後のD-Busメソッド呼び出しから本値を超えてアクセスが無い場合、サービスプロセスは自律的に終了する。
     */
    static constexpr int IdleTimeoutMs = 5 * 60 * 1000;

    /**
     * @brief 同時に保持できるpolkitプロンプト待ちの上限
     *
     * プロンプトはタイムアウトを持たないため、未応答のまま滞留し得る。
     * 1呼び出しあたりの保持量は小さいが、無制限に積ませない
     */
    static constexpr int MaxPendingAuthorizations = 8;
    std::unique_ptr<snapper::Snapper> m_snapper;        // 現在のSnapperインスタンス
    // m_comparisonCache は m_snapper の後に宣言 (デストラクション順序:
    // Comparisonが所有するmounts/FilesがSnapperより先に破棄されるようにするため)
    ComparisonCache<snapper::Comparison> m_comparisonCache;
    QString m_currentConfig;                            // 現在選択中のSnapper設定名
    QTimer m_idleTimer;                                 // アイドルタイムアウト用タイマ
    qsnapper::restore::RestoreManifestRegistry m_restoreRegistry;
    qsnapper::restore::RestorePlanExecutor m_restoreExecutor;
    QDBusServiceWatcher *m_ownerWatcher = nullptr;
    QMap<QString, RestoreExecution> m_restoreExecutions;
    QMap<QString, QString> m_restorePlanOwners;
    // 遅延応答の継続を実行している間のみ有効。
    // replyError()がsendErrorReply()ではなくcapture済みmessageを使う判断に用いる
    std::optional<CallReply> m_deferredReply;
    int m_pendingAuthorizations = 0;                    // polkitプロンプト待ちの件数

private:
    /**
     * @brief アイドルタイマをリセットする
     *
     * D-Busメソッドの先頭で呼び出し、無操作5分による自動終了を先送りする。
     * 認可待ちが1件でもある間はタイマを止めたままにする (プロンプト応答を待つ間に
     * サービスが自動終了すると、ユーザが認証した直後に呼び出しが失われるため)
     */
    void resetIdleTimer();

    /**
     * @brief 現在のD-Bus呼び出しの応答contextをcaptureする
     *
     * message()はスロットから戻ると無効になるため、認可待ちを跨ぐ経路では
     * 本関数の戻り値を保持して応答する
     *
     * @return capture済み応答context (D-Bus経由でなければfromDBus=false)
     */
    CallReply captureCallReply() const;

    /**
     * @brief 同期応答と遅延応答のどちらでも正しくD-Busエラーを返す
     *
     * 継続実行中 (m_deferredReplyが有効) はcapture済みmessageから
     * createErrorReply()して送出し、それ以外はQDBusContext::sendErrorReply()に委ねる
     *
     * @param type 返すD-Busエラー種別
     * @param text エラーメッセージ
     */
    void replyError(QDBusError::ErrorType type, const QString &text);

    /**
     * @brief polkit認可を要求する (対話が必要な場合のみ非同期化する)
     *
     * 高速経路として対話を許可しない問い合わせを先に行う。allow_active=yesや
     * auth_admin_keepのキャッシュ済み認可はここでGrantedとなり、プロンプトが
     * 出ないためevent loopはミリ秒しか止まらない。
     * 対話が必要 (challenge) な場合のみ非同期APIへ回し、setDelayedReply(true)を
     * 立てた上でDeferredを返す。同期版はタイムアウトを持たず、未応答プロンプト
     * 1つでサービス全体のevent loopが無期限に凍結するため、対話経路では使わない
     *
     * @param actionId Polkitアクションid
     * @param continuation 認可完了時に呼ぶ継続 (capture済み応答contextと可否を受け取る)
     * @return 即時許可 / 即時拒否 / 遅延のいずれか
     */
    AuthorizationOutcome beginAuthorization(
        const QString &actionId,
        std::function<void(const CallReply &, bool)> continuation);

    /**
     * @brief 認可待ち1件の終了を記録し、必要ならアイドルタイマを再開する
     */
    void endPendingAuthorization();

    /**
     * @brief 遅延応答経路で本体を実行し、戻り値をD-Bus応答として送出する
     *
     * 本体が既にreplyError()でエラーを返している場合は値応答を送らない
     *
     * @tparam T 対象D-Busメソッドの戻り値型
     * @param reply capture済み応答context
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
     * 対話不要ならその場で本体を同期実行して戻り値を返す (従来と同じ挙動)。
     * 対話が必要な場合は遅延応答へ切り替え、既定値を返して呼び出しを終える
     *
     * @tparam T 対象D-Busメソッドの戻り値型
     * @param actionId Polkitアクションid
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
     * 空文字列の入力は "root" に正規化した上で、qsnapper::security::validateConfigNameで検証する
     * これにより呼び出し側の "空なら root" デフォルト割当パターンが不要になり、
     * 空入力に対する一貫した扱い (常に "root" として受理) を保証する
     *
     * 各D-Busスロットの先頭 (beginAuthorization より前) で呼び出すこと
     * Polkitプロンプトが出てから "invalid config name" で蹴られるUXを避けるため順序が重要
     *
     * @param configName 検査対象の設定名 (空文字列は "root" として扱う)
     * @return 正規化後の設定名 (有効な場合)、無効でエラー応答済みなら std::nullopt
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
     * @param filePath 検証済み絶対path
     */
    QString getFileDiffAndDetailsAuthorized(const QString &configName,
                                            int snapshotNumber,
                                            const QString &filePath);

    /**
     * @brief 認可済みのGetFileDiffBetween本体
     * @param configName 検証済み設定名
     * @param filePath 検証済み絶対path
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
     * 認可待ちの間にcancel / TTL失効 / owner消失 / 他計画の実行開始が
     * 起こり得るため、mountやexecutor起動の前に状態を再検証する
     *
     * @param manifestId owner束縛されたmanifest id
     * @param owner 認可前にcaptureした呼び出し元unique name
     * @return 実行開始を受理した場合true
     */
    bool commitRestorePlanAuthorized(const QString &manifestId,
                                     const QString &owner);

    /**
     * @brief 認可が得られなかった復元計画をFailedで終端する
     * @param manifestId 対象manifest id
     * @param owner 認可前にcaptureした呼び出し元unique name
     */
    void failRestorePlanAuthorization(const QString &manifestId,
                                      const QString &owner);

    /**
     * @brief Snapperインスタンスを取得 (必要に応じて生成/再生成)
     * @param configName 設定名
     * @param forceReload 強制再生成フラグ
     */
    snapper::Snapper* getSnapper(const QString &configName = "root",
                                 bool forceReload = false);

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
     * @brief RestoreFiles / RestoreFilesDirect 共通実装
     *
     * 両エントリポイントは差分フラグを引数にして本関数へ委譲する
     * 入力パスはqsnapper::security::isPathWithinSnapshotRootでsnapshot root内に収まっていることを検証する
     *
     * @param configName      Snapper設定名
     * @param snapshotNumber  復元元スナップショット番号
     * @param filePaths       復元対象ファイルの絶対パス
     * @param changeTypes     各ファイルのchange種別
     * @param useReflink      通常ファイルコピー時にFICLONE (btrfs CoW) を試行するか
     * @param removeOnTypechanged typechanged時に既存ファイルを先に削除するか
     * @param logTag          ログ前置詞 ("RestoreFiles"など)
     */
    bool restoreFilesImpl(const QString &configName,
                          int snapshotNumber,
                          const QStringList &filePaths,
                          const QStringList &changeTypes,
                          bool useReflink,
                          bool removeOnTypechanged,
                          const char *logTag);

    /**
     * @brief 認可済みのrestoreFilesImpl本体
     *
     * 入力検証 (configName / filePaths) はrestoreFilesImpl側で認可前に完了している
     *
     * @param configName 検証済み設定名
     */
    bool restoreFilesAuthorized(const QString &configName,
                                int snapshotNumber,
                                const QStringList &filePaths,
                                const QStringList &changeTypes,
                                bool useReflink,
                                bool removeOnTypechanged,
                                const char *logTag);

    /**
     * @brief 通常ファイルをコピーする (オーナー/時刻保持)
     * @param tryReflink FICLONE (reflink) を先行試行するか
     */
    static bool copyRegularFile(const QString &src,
                                const QString &dst,
                                bool tryReflink);

    /**
     * @brief シンボリックリンクをコピーする (リンク先/オーナー/時刻保持)
     */
    static bool copySymlink(const QString &src,
                             const QString &dst);

    /**
     * @brief live宛先をroot配下で再解決して通常ファイルをコピーする
     *
     * ソースはpin済みsnapshot dirfd相対で解決する。実行中にsnapshotDirの
     * path上で削除 / 再作成 / 差し替えが起きても、fdが指すinodeを読み続けるため、
     * 認可時と異なる内容を読み込むことがない
     *
     * @param sourceDirFd pin済みのsnapshot dirfd
     * @param sourceRelativePath snapshotDirからの相対source path (絶対パス・".."/"."成分は不可)
     * @param dst live filesystem上の絶対path
     * @param tryReflink FICLONEを先行試行するか
     * @return dataと必須metadataを適用できた場合true
     */
    static bool copyRegularFileBeneathRoot(int sourceDirFd,
                                           const QString &sourceRelativePath,
                                           const QString &dst,
                                           bool tryReflink);

    /**
     * @brief live宛先をroot配下で再解決してsymlinkをコピーする
     *
     * ソースはpin済みsnapshot dirfd相対で解決する (copyRegularFileBeneathRootと同じ理由)
     *
     * @param sourceDirFd pin済みのsnapshot dirfd
     * @param sourceRelativePath snapshotDirからの相対source path
     * @param dst live filesystem上の絶対path
     * @return symlinkを作成できた場合true
     */
    static bool copySymlinkBeneathRoot(int sourceDirFd,
                                       const QString &sourceRelativePath,
                                       const QString &dst);

    /**
     * @brief live pathをroot配下で再解決して一時的な兄弟pathへ退避する
     * @param path 退避対象の絶対path
     * @param movedPath 実際の退避先。対象不存在時は空文字列
     * @return 退避または対象不存在時true
     */
    static bool movePathAsideBeneathRoot(const QString &path,
                                         QString *movedPath);

    /**
     * @brief 現在のD-Bus呼び出し元unique nameを返す
     * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
     */
    QString callerOwner() const;

    /**
     * @brief manifest操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
     * @param error registryが返したエラー
     * @return 常にfalse
     */
    bool sendManifestError(qsnapper::restore::ManifestError error);

    /**
     * @brief manifest状態をD-Bus contractの小文字表現へ変換する
     * @param state 変換対象状態
     * @return staging/frozen/running/completed/failed/cancelledのいずれか
     */
    static QString restoreManifestStateString(
        qsnapper::restore::ManifestState state);

    /**
     * @brief 復元方式をD-Bus contractの文字列表現へ変換する
     * @param mode 変換対象方式
     * @return yastまたはdirect
     */
    static QString restoreModeString(qsnapper::restore::RestoreMode mode);

    /**
     * @brief RFC4180形式で必要なCSV fieldをquoteする
     *
     * commaまたはdouble quoteを含むfieldはdouble quoteで囲み、内部の
     * double quoteを二重化する。
     *
     * @param field quote対象文字列
     * @return CSVへ安全に埋め込めるfield
     */
    static QString quoteRestoreStatusCsvField(const QString &field);

    /**
     * @brief executor callbackから単一の凍結済みentryをlive filesystemへ適用する
     * @param manifestId 実行contextを選択するmanifest id
     * @param entry 適用対象entry
     * @return entry全体の適用成功時true
     */
    bool applyRestoreEntry(const QString &manifestId,
                           const qsnapper::restore::RestoreEntry &entry);

    /**
     * @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
     * @param manifestId 終端したmanifest id
     * @param terminal 終端状態
     * @param message 終端理由
     */
    void finishRestorePlan(const QString &manifestId,
                           qsnapper::restore::ManifestState terminal,
                           const QString &message);

    /**
     * @brief 指定manifestのmountを可能な限り解除して実行contextを削除する
     * @param manifestId cleanup対象manifest id
     */
    void cleanupRestoreExecution(const QString &manifestId);

    /**
     * @brief execution contextに記録されたsnapshot mountを解除する
     * @param execution mount元設定とsnapshot番号を持つcontext
     */
    void unmountRestoreExecution(const RestoreExecution &execution);

    /**
     * @brief owner消失時に予約済み実行とmountとmanifestを全て破棄する
     * @param owner unregisterされたD-Bus unique name
     */
    void handleRestoreOwnerUnregistered(const QString &owner);

    /**
     * @brief TTL purgeで消えたactive計画をabandonしmountもcleanupする
     */
    void purgeExpiredRestorePlans();

    /**
     * @brief manifestを持たないownerをservice watcherから除外する
     */
    void removeUnusedRestoreOwnerWatches();

    /**
     * @brief 復元後にroot subvolumeがread-onlyならrwへ戻す安全ネットを実行する
     */
    static void restoreRootReadWriteSafetyNet();

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

public slots:
    /**
     * @brief 既存の Snapper 設定名の一覧を返す
     * @return 設定名の配列
     */
    QStringList ListConfigs();

    /**
     * @brief 指定設定のスナップショット一覧をCSVで返す
     * @param configName 設定名 (空文字列時は "root")
     */
    QString ListSnapshots(const QString &configName);

    /**
     * @brief 新しいスナップショットを作成する
     * @param configName 設定名
     * @param type "single" / "pre" / "post"
     * @param description 説明
     * @param preNumber postタイプ時の対応pre番号
     * @param cleanup cleanup アルゴリズム名
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
     * @brief ファイルをスナップショットから復元する (YaST互換コピー経路)
     *
     * reflinkを用いず、typechangedの事前削除も行わない。
     */
    bool RestoreFiles(const QString &configName,
                      int snapshotNumber,
                      const QStringList &filePaths,
                      const QStringList &changeTypes);

    /**
     * @brief ファイルをスナップショットから復元する (高速経路)
     *
     * btrfs reflinkを優先、typechangedは既存ファイル削除後にコピー。
     */
    bool RestoreFilesDirect(const QString &configName,
                             int snapshotNumber,
                             const QStringList &filePaths,
                             const QStringList &changeTypes);

    /**
     * @brief ownerに束縛された空のstaged restore計画を開始する
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元snapshot番号
     * @param restoreMode yastまたはdirect
     * @return 成功時manifest id
     */
    QString BeginRestorePlan(const QString &configName,
                             int snapshotNumber,
                             const QString &restoreMode);

    /**
     * @brief staging計画へ検証済みentry chunkを原子的に追加する
     * @param manifestId owner束縛されたmanifest id
     * @param filePaths 復元対象絶対path列
     * @param changeTypes pathと対応する変更種別列
     * @return chunk全体を追加できた場合true
     */
    bool StageRestoreEntries(const QString &manifestId,
                             const QStringList &filePaths,
                             const QStringList &changeTypes);

    /**
     * @brief 計画をfreeze後に1度だけ認可し非同期実行を開始する
     * @param manifestId owner束縛されたmanifest id
     * @return 実行開始を受理した場合true
     */
    bool CommitRestorePlan(const QString &manifestId);

    /**
     * @brief owner確認済みRunning計画のidle loopを再開する
     * @param manifestId owner束縛されたmanifest id
     * @return nudgeを受理した場合true
     */
    bool ContinueRestorePlan(const QString &manifestId);

    /**
     * @brief owner確認済み計画状態をRFC4180 escaping済みCSVで返す
     * @param manifestId owner束縛されたmanifest id
     * @return ManifestStatus field順のCSV、失敗時空文字列
     */
    QString GetRestorePlanStatus(const QString &manifestId);

    /**
     * @brief owner確認済み非終端計画へ境界cancellationを要求する
     * @param manifestId owner束縛されたmanifest id
     * @return cancellationを受理した場合true
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
     * @brief 復元処理の進捗通知
     * @param current 現在処理中のファイル番号 (1から始まる)
     * @param total 総ファイル数
     * @param filePath 処理中のファイルパス
     */
    void restoreProgress(int current,
                          int total,
                          const QString &filePath);

    /**
     * @brief staged restore計画の進捗通知
     * @param manifestId 実行中計画id
     * @param current 完了entry数
     * @param total 凍結時の総entry数
     * @param filePath 情報漏洩を抑えたbasename
     */
    void restorePlanProgress(const QString &manifestId,
                             int current,
                             int total,
                             const QString &filePath);

    /**
     * @brief staged restore計画の終端通知
     * @param manifestId 終端した計画id
     * @param terminalState completed/failed/cancelledのいずれか
     * @param message 終端理由
     */
    void restorePlanFinished(const QString &manifestId,
                             const QString &terminalState,
                             const QString &message);
};

#endif // SNAPSHOTOPERATIONS_H
