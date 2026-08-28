#ifndef QSNAPPER_RESTOREMANIFEST_H
#define QSNAPPER_RESTOREMANIFEST_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace qsnapper::restore {

/**
 * @brief 復元manifestの単調なライフサイクル状態
 */
enum class ManifestState {
    Staging,
    Frozen,
    Running,
    Completed,
    Failed,
    Cancelled
};

/**
 * @brief manifestに固定される復元方式
 */
enum class RestoreMode {
    YastCompatible,
    DirectCopy
};

/**
 * @brief manifest操作の機械可読エラー
 */
enum class ManifestError {
    None,
    NotFound,
    OwnerMismatch,
    WrongState,
    Expired,
    CapacityExceeded,
    InvalidArgument,
    AlreadyTerminal,
    GlobalLimit
};

/**
 * @brief 順序を保持する単一の復元対象
 */
struct RestoreEntry {
    QString path;
    QString changeType;
};

/**
 * @brief 外部へ公開できるmanifestの状態スナップショット
 */
struct ManifestStatus {
    QString id;
    ManifestState state = ManifestState::Staging;
    int totalEntries = 0;
    int cursor = 0;
    int processed = 0;
    RestoreMode mode = RestoreMode::YastCompatible;
    QString configName;
    int snapshotNumber = -1;
    QString lastError;
};

/**
 * @brief 単一復元計画のD-Bus非依存な状態機械
 *
 * owner、設定、snapshot、mode、id、TTLは構築後不変である。freeze後の
 * entry列はconstなコンテナへ移され、型として変更できない。
 */
class RestoreManifest
{
public:
    /**
     * @brief Staging状態の復元計画を構築する
     * @param owner D-Bus呼び出し元のunique name
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元snapshot番号
     * @param mode 復元方式
     * @param id 推測困難なmanifest識別子
     * @param creationTimeMs 作成時刻 (milliseconds)
     * @param ttlMs 最終活動から失効するまでの時間 (milliseconds)
     */
    RestoreManifest(QString owner, QString configName, int snapshotNumber,
                    RestoreMode mode, QString id, qint64 creationTimeMs,
                    qint64 ttlMs);

    /**
     * @brief Staging中の末尾へentry列を原子的に追加する
     * @param entries 追加する順序付きentry列
     * @param err 結果エラーの格納先 (省略可)
     * @return 追加成功時true
     */
    bool appendEntries(const QVector<RestoreEntry> &entries, ManifestError *err);

    /**
     * @brief 非空のStaging計画を変更不能なFrozen状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool freeze(ManifestError *err);

    /**
     * @brief Frozen計画を1度だけRunning状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markRunning(ManifestError *err);

    /**
     * @brief Running計画のcursorを進め、終端到達時にCompletedへ遷移する
     * @param count 進める正のentry数
     * @param err 結果エラーの格納先 (省略可)
     * @return 更新成功時true
     */
    bool advanceCursor(int count, ManifestError *err);

    /**
     * @brief 非終端計画をFailed状態へ遷移する
     * @param reason 人間向けの失敗理由
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markFailed(const QString &reason, ManifestError *err);

    /**
     * @brief 非終端計画をCancelled状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool cancel(ManifestError *err);

    /**
     * @brief 計画がCompleted、Failed、Cancelledのいずれかを返す
     * @return 終端状態ならtrue
     */
    bool isTerminal() const;

    /**
     * @brief 指定時刻でTTLを超過しているか判定する
     *
     * Running状態の計画は認可済みかつexecutorが駆動中であり、TTL回収の対象外とする
     * (owner消失時はregistryのremoveByOwnerが回収する)。
     *
     * @param nowMs 判定時刻 (milliseconds)
     * @return 非Runningかつ最終活動からTTLを超過していればtrue
     */
    bool isExpired(qint64 nowMs) const;

    /**
     * @brief 最終活動時刻を更新する
     * @param nowMs 新しい最終活動時刻 (milliseconds)
     */
    void touch(qint64 nowMs);

    /**
     * @brief 所有者unique nameを返す
     * @return 構築時に固定された所有者
     */
    const QString &owner() const;

    /**
     * @brief manifest識別子を返す
     * @return 構築時に固定された識別子
     */
    const QString &id() const;

    /**
     * @brief 現在の状態を返す
     * @return manifest状態
     */
    ManifestState state() const;

    /**
     * @brief Snapper設定名を返す
     * @return 構築時に固定された設定名
     */
    const QString &configName() const;

    /**
     * @brief 復元元snapshot番号を返す
     * @return 構築時に固定されたsnapshot番号
     */
    int snapshotNumber() const;

    /**
     * @brief 復元方式を返す
     * @return 構築時に固定された復元方式
     */
    RestoreMode mode() const;

    /**
     * @brief 保存済みentry総数を返す
     * @return entry総数
     */
    int totalEntries() const;

    /**
     * @brief 次に処理するentry位置を返す
     * @return 0始まりのcursor
     */
    int cursor() const;

    /**
     * @brief 指定位置のentryを値として返す
     * @param index 0始まりの位置
     * @return 範囲内ならentry、範囲外ならstd::nullopt
     */
    std::optional<RestoreEntry> entryAt(int index) const;

    /**
     * @brief 指定範囲のentryを順序通り値として返す
     * @param offset 0始まりの開始位置
     * @param count 最大取得数
     * @return 不正範囲では空、それ以外では切り出したentry列
     */
    QVector<RestoreEntry> entriesSlice(int offset, int count) const;

    /**
     * @brief 外部公開用の状態スナップショットを返す
     * @return 現在状態を反映したManifestStatus
     */
    ManifestStatus status() const;

private:
    const QVector<RestoreEntry> &entries() const;
    static void setError(ManifestError *err, ManifestError value);

    const QString m_owner;
    const QString m_configName;
    const int m_snapshotNumber;
    const RestoreMode m_mode;
    const QString m_id;
    const qint64 m_creationTimeMs;
    const qint64 m_ttlMs;
    ManifestState m_state = ManifestState::Staging;
    QVector<RestoreEntry> m_stagingEntries;
    std::unique_ptr<const QVector<RestoreEntry>> m_frozenEntries;
    int m_cursor = 0;
    qint64 m_lastActivityMs;
    QString m_lastError;

    Q_DISABLE_COPY(RestoreManifest)
};

/**
 * @brief owner束縛・TTL・容量上限を強制して復元計画を所有するregistry
 *
 * 単一Qt event-loopスレッドから利用する非thread-safeな所有コンテナである。
 */
class RestoreManifestRegistry
{
public:
    static constexpr int kMaxEntriesPerManifest = 200000;
    static constexpr qint64 kMaxPathBytesPerManifest = 64 * 1024 * 1024;
    static constexpr int kMaxEntriesPerStageChunk = 5000;
    static constexpr int kMaxManifestsPerOwner = 4;
    static constexpr int kMaxManifestsGlobal = 32;
    static constexpr qint64 kDefaultTtlMs = 10 * 60 * 1000;

    // staging (BeginRestorePlan / StageRestoreEntries) は設計上、認可を要さない。
    // manifest単位の上限だけでは、攻撃者がD-Bus接続を複数開くだけで
    // (unique nameごとに別ownerとして数えられるため) 上限を掛け算でき、
    // rootサービスに 32 manifest x 64MB = 2GB を確保させられた。
    // 以下のグローバル予算は、その掛け算をプロセス全体で1回に閉じる。
    //
    // 単位はUTF-8 path byteとentry数の2軸を持つ。path byteだけでは
    // 「1文字pathを大量にstageする」経路 (entry 1件あたりQString 2本と
    //  QVector slotの固定費が乗る) を縛れないため、両方を強制する。
    // 値は「最大構成の計画1件 + 別途staging中の1件」が通る大きさに取ってある
    static constexpr qint64 kMaxEntriesGlobal = 2 * qint64(kMaxEntriesPerManifest);
    static constexpr qint64 kMaxPathBytesGlobal = 2 * kMaxPathBytesPerManifest;

    /**
     * @brief 実時間clockを使用する空のregistryを構築する
     */
    RestoreManifestRegistry();

    /**
     * @brief TTL判定と活動更新に使うclockを差し替える
     * @param clock millisecondsを返す関数。空なら実時間clockへ戻す
     */
    void setClock(std::function<qint64()> clock);

    /**
     * @brief テスト専用にentry数・path byte上限を縮小する
     * @param maxEntriesPerManifest 正のmanifest単位entry数上限
     * @param maxPathBytesPerManifest 非負のmanifest単位UTF-8 path byte上限
     * @param maxEntriesGlobal 正のregistry全体entry数上限
     * @param maxPathBytesGlobal 非負のregistry全体UTF-8 path byte上限
     *
     * productionの呼び出し側では使用しない。固定の公開上限を超える値は
     * 固定上限へ丸められる。
     */
    void setCapacityOverridesForTesting(
        int maxEntriesPerManifest,
        qint64 maxPathBytesPerManifest,
        qint64 maxEntriesGlobal = kMaxEntriesGlobal,
        qint64 maxPathBytesGlobal = kMaxPathBytesGlobal);

    /**
     * @brief ownerに束縛された空のStaging計画を作成する
     * @param owner D-Bus呼び出し元のunique name
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元snapshot番号
     * @param mode 復元方式
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時は推測困難なid、拒否時は空文字列
     */
    QString createStaging(const QString &owner, const QString &configName,
                          int snapshotNumber, RestoreMode mode,
                          ManifestError *err);

    /**
     * @brief owner確認後にpath/changeType列を原子的にstageする
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param paths path列
     * @param changeTypes pathsと同数の変更種別列
     * @param err 結果エラーの格納先 (省略可)
     * @return 追加成功時true
     */
    bool stageEntries(const QString &id, const QString &owner,
                      const QStringList &paths, const QStringList &changeTypes,
                      ManifestError *err);

    /**
     * @brief owner確認後にmanifestをfreezeする
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool freeze(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にmanifestをRunningへ遷移する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markRunning(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にmanifestのcursorを進める
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param count 進める正のentry数
     * @param err 結果エラーの格納先 (省略可)
     * @return 更新成功時true
     */
    bool advance(const QString &id, const QString &owner, int count,
                 ManifestError *err);

    /**
     * @brief owner確認済みmanifestのTTLだけを延長する
     *
     * 状態もcursorも変更せず、m_lastActivityMsのみ更新する。
     * 長時間かかる単一chunkの実行中にidle TTLが誤って満了するのを防ぐ。
     *
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return TTLを延長できた場合true
     */
    bool keepAlive(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にmanifestをFailedへ遷移する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param reason 人間向けの失敗理由
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markFailed(const QString &id, const QString &owner,
                    const QString &reason, ManifestError *err);

    /**
     * @brief owner確認後にmanifestをCancelledへ遷移する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool cancel(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にmanifest状態を取得する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時は状態、拒否時はstd::nullopt
     */
    std::optional<ManifestStatus> status(const QString &id,
                                         const QString &owner,
                                         ManifestError *err);

    /**
     * @brief owner確認後にexecutor用entry範囲を取得する
     * @param id manifest識別子
     * @param owner 呼び出し元owner
     * @param offset 0始まりの開始位置
     * @param count 最大取得数
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時はentry列、拒否時はstd::nullopt
     */
    std::optional<QVector<RestoreEntry>> entriesSlice(const QString &id,
                                                       const QString &owner,
                                                       int offset, int count,
                                                       ManifestError *err);

    /**
     * @brief 消失したownerに属するmanifestを全て削除する
     * @param owner 削除対象owner
     * @return 削除したmanifest数
     */
    int removeByOwner(const QString &owner);

    /**
     * @brief 現在時刻で失効したmanifestを全て削除する
     * @return 削除したmanifest数
     */
    int purgeExpired();

    /**
     * @brief idに一致するmanifestを所有者確認なしで内部削除する
     * @param id manifest識別子
     * @return 存在して削除した場合true
     */
    bool remove(const QString &id);

    /**
     * @brief registry内のmanifest総数を返す
     * @return manifest総数
     */
    int count() const;

    /**
     * @brief 指定ownerに属するmanifest数を返す
     * @param owner 集計対象owner
     * @return 一致するmanifest数
     */
    int countForOwner(const QString &owner) const;

    /**
     * @brief 全manifestが保持するUTF-8 path byteの合計を返す
     *
     * 走査対象は高々kMaxManifestsGlobal件であり、実行中の累算値を
     * 4箇所ある削除経路と同期させるよりも、都度集計する方が破綻しない
     *
     * @return path byte合計
     */
    qint64 globalPathBytes() const;

    /**
     * @brief 全manifestが保持するentry数の合計を返す
     * @return entry数合計
     */
    qint64 globalEntries() const;

private:
    struct ManifestRecord {
        std::unique_ptr<RestoreManifest> manifest;
        qint64 pathBytes = 0;
    };

    using ManifestMap = std::map<QString, ManifestRecord>;

    RestoreManifest *findOwned(const QString &id, const QString &owner,
                               ManifestError *err);
    static void setError(ManifestError *err, ManifestError value);
    static qint64 defaultNowMs();

    ManifestMap m_manifests;
    std::function<qint64()> m_clock;
    int m_maxEntriesPerManifest = kMaxEntriesPerManifest;
    qint64 m_maxPathBytesPerManifest = kMaxPathBytesPerManifest;
    qint64 m_maxEntriesGlobal = kMaxEntriesGlobal;
    qint64 m_maxPathBytesGlobal = kMaxPathBytesGlobal;
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREMANIFEST_H
