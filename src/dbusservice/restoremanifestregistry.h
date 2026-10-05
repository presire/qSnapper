/**
 * @file restoremanifestregistry.h
 * @brief 復元計画の所有と上限を管理するregistryと終端記録store
 *
 * RestoreManifest の定義は restoremanifest.h から取り込む
 * 単独で取り込んでも所有管理まで利用できる
 */

#ifndef QSNAPPER_RESTOREMANIFESTREGISTRY_H
#define QSNAPPER_RESTOREMANIFESTREGISTRY_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <map>

#include "restoremanifest.h"

// 復元計画の所有と上限の管理を扱う名前空間
namespace qsnapper::restore {

/**
 * @class RestoreManifestRegistry
 * @brief owner束縛とTTLと容量上限を強制して復元計画を所有するregistry
 *
 * 単一Qtイベントループスレッドから利用する非スレッドセーフな所有コンテナである
 *
 * m_manifests : 識別子を鍵とする計画と予算記録の所有map
 * m_clock : TTL判定と活動更新に使う時刻源 (差し替え可能)
 * m_maxEntriesPerManifest : マニフェスト単位のエントリ数上限
 * m_maxPathBytesPerManifest : マニフェスト単位のUTF-8 pathバイト数上限
 * m_maxEntriesGlobal : registry全体のエントリ数上限
 * m_maxPathBytesGlobal : registry全体のUTF-8 pathバイト数上限
 * m_maxEntriesPerUid : UID単位のエントリ数上限
 * m_maxPathBytesPerUid : UID単位のUTF-8 pathバイト数上限
 */
class RestoreManifestRegistry
{
public:
    // マニフェスト単位の上限
    static constexpr int kMaxEntriesPerManifest = 200000;              // 1計画の最大エントリ数
    static constexpr qint64 kMaxPathBytesPerManifest = 64 * 1024 * 1024; // 1計画の最大UTF-8 pathバイト数
    static constexpr int kMaxEntriesPerStageChunk = 5000;              // 1回のstage受け付け上限
    static constexpr int kMaxManifestsPerOwner = 4;                     // owner単位の最大計画数
    static constexpr int kMaxManifestsGlobal = 32;                     // 全体の最大計画数
    static constexpr qint64 kDefaultTtlMs = 10 * 60 * 1000;             // 既定のTTL (ミリ秒)

    // staging ([BeginRestorePlan] / [StageRestoreEntries]) は設計上、認可を要さない
    // マニフェスト単位の上限だけでは、攻撃者がD-Bus接続を複数開くだけで (ユニーク名ごとに別ownerとして数えられるため) 上限を掛け算でき、
    // rootサービスに [32マニフェスト x 64MB = 2GB] を確保させられる
    //
    // 以下のグローバル予算は、その掛け算をプロセス全体で1回に閉じる
    //
    // 単位はUTF-8 pathバイト数とエントリ数の2軸を持つ
    // pathバイト数だけでは [1文字pathを大量にstageする] 経路 (エントリ1件あたりQString 2本とQVector要素の固定費が乗る) を縛れないため、両方を強制する
    // 値は [最大構成の計画1件と別途staging中の1件] が通る大きさに取ってある
    static constexpr qint64 kMaxEntriesGlobal = 2 * qint64(kMaxEntriesPerManifest); // 全体の最大エントリ数
    static constexpr qint64 kMaxPathBytesGlobal = 2 * kMaxPathBytesPerManifest;     // 全体の最大UTF-8 pathバイト数

    // UID単位の予算
    // ownerはD-Busのユニーク名単位であるため、1人のユーザが接続を複数開けば、owner単位の上限を掛け算してグローバル予算を使い切れる
    // (例: 8接続 x 4計画で全体の32計画を占有し、正規の復元をGlobalLimitで阻止できる)
    // 呼び出し元のUIDごとにも上限を設け、1人のユーザが使えるのはグローバル予算の一部までとする
    // 値は [最大構成の計画1件] が通る大きさで、残りのグローバル予算は他のユーザの復元に残る
    static constexpr int kMaxManifestsPerUid = kMaxManifestsPerOwner;  // UID単位の最大計画数
    static constexpr qint64 kMaxEntriesPerUid = qint64(kMaxEntriesPerManifest); // UID単位の最大エントリ数
    static constexpr qint64 kMaxPathBytesPerUid = kMaxPathBytesPerManifest; // UID単位の最大UTF-8 pathバイト数

    /**
     * @brief 実時間clockを使用する空のregistryを構築する
     */
    RestoreManifestRegistry();

    /**
     * @brief TTL判定と活動更新に使うclockを差し替える
     * @param clock ミリ秒を返す関数
     *              空なら実時間clockへ戻す
     */
    void setClock(std::function<qint64()> clock);

    /**
     * @brief テスト専用にエントリ数とpathバイト数上限を縮小する
     * @param maxEntriesPerManifest 正のマニフェスト単位エントリ数上限
     * @param maxPathBytesPerManifest 非負のマニフェスト単位UTF-8 pathバイト数上限
     * @param maxEntriesGlobal 正のregistry全体エントリ数上限
     * @param maxPathBytesGlobal 非負のregistry全体UTF-8 pathバイト数上限
     * @param maxEntriesPerUid 正のUID単位エントリ数上限
     * @param maxPathBytesPerUid 非負のUID単位UTF-8 pathバイト数上限
     *
     * productionの呼び出し側では使用しない
     * 固定の公開上限を超える値は固定上限へ丸められる
     */
    void setCapacityOverridesForTesting(
        int maxEntriesPerManifest,
        qint64 maxPathBytesPerManifest,
        qint64 maxEntriesGlobal = kMaxEntriesGlobal,
        qint64 maxPathBytesGlobal = kMaxPathBytesGlobal,
        qint64 maxEntriesPerUid = kMaxEntriesPerUid,
        qint64 maxPathBytesPerUid = kMaxPathBytesPerUid);

    // owner束縛を変更する操作系
    /**
     * @brief ownerに束縛された空のStaging計画を作成する
     * @param owner D-Bus呼び出し元のユニーク名
     * @param ownerUid 呼び出し元のUID (UID単位の予算に使用する)
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号 (0は現在のシステム)
     * @param mode 復元方式
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時は推測困難なID、拒否時は空文字列
     */
    QString createStaging(const QString &owner, uint ownerUid,
                          const QString &configName, int snapshotNumber,
                          int counterpartSnapshotNumber, RestoreMode mode,
                          ManifestError *err);

    /**
     * @brief owner確認後にpathとchangeType列を原子的にstageする
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param paths path列
     * @param changeTypes pathsと同数の変更種別列
     * @param err 結果エラーの格納先 (省略可)
     * @return 追加成功時true
     */
    bool stageEntries(const QString &id, const QString &owner,
                      const QStringList &paths, const QStringList &changeTypes,
                      ManifestError *err);

    // owner束縛の状態遷移系
    /**
     * @brief owner確認後にマニフェストをfreezeする
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool freeze(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にマニフェストをRunningへ遷移する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markRunning(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にマニフェストのcursorを進める
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param count 進める正のエントリ数
     * @param err 結果エラーの格納先 (省略可)
     * @return 更新成功時true
     */
    bool advance(const QString &id, const QString &owner, int count,
                 ManifestError *err);

    /**
     * @brief owner確認済みマニフェストのTTLだけを延長する
     *
     * 状態もcursorも変更せず、m_lastActivityMsのみ更新する
     * 長時間かかる単一chunkの実行中にidle TTLが誤って満了するのを防ぐ
     *
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return TTLを延長できた場合true
     */
    bool keepAlive(const QString &id, const QString &owner, ManifestError *err);

    /**
     * @brief owner確認後にマニフェストをFailedへ遷移する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param reason 人間向けの失敗理由
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markFailed(const QString &id, const QString &owner,
                    const QString &reason, ManifestError *err);

    /**
     * @brief owner確認後にマニフェストをCancelledへ遷移する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool cancel(const QString &id, const QString &owner, ManifestError *err);

    // owner束縛の参照系
    /**
     * @brief owner確認後にマニフェスト状態を取得する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時は状態、拒否時はstd::nullopt
     */
    std::optional<ManifestStatus> status(const QString &id,
                                         const QString &owner,
                                         ManifestError *err);

    /**
     * @brief owner確認後にexecutor用エントリ範囲を取得する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param offset 0始まりの開始位置
     * @param count 最大取得数
     * @param err 結果エラーの格納先 (省略可)
     * @return 成功時はエントリ列、拒否時はstd::nullopt
     */
    std::optional<QVector<RestoreEntry>> entriesSlice(const QString &id,
                                                       const QString &owner,
                                                       int offset, int count,
                                                       ManifestError *err);

    // 回収と集計系
    /**
     * @brief 消失したownerに属するマニフェストを全て削除する
     * @param owner 削除対象owner
     * @return 削除したマニフェスト数
     */
    int removeByOwner(const QString &owner);

    /**
     * @brief 現在時刻で失効したマニフェストを全て削除する
     * @return 削除したマニフェスト数
     */
    int purgeExpired();

    /**
     * @brief idに一致するマニフェストを所有者確認なしで内部削除する
     * @param id マニフェスト識別子
     * @return 存在して削除した場合true
     */
    bool remove(const QString &id);

    /**
     * @brief registry内のマニフェスト総数を返す
     * @return マニフェスト総数
     */
    int count() const;

    /**
     * @brief 指定ownerに属するマニフェスト数を返す
     * @param owner 集計対象owner
     * @return 一致するマニフェスト数
     */
    int countForOwner(const QString &owner) const;

    /**
     * @brief 指定UIDに属するマニフェスト数を返す
     * @param ownerUid 集計対象UID
     * @return 一致するマニフェスト数
     */
    int countForUid(uint ownerUid) const;

    /**
     * @brief 全マニフェストが保持するUTF-8 pathバイト数の合計を返す
     *
     * 走査対象は高々kMaxManifestsGlobal件であり、
     * 実行中の累算値を4箇所ある削除経路と同期させるよりも都度集計する方が破綻しない
     *
     * @return pathバイト数合計
     */
    qint64 globalPathBytes() const;

    /**
     * @brief 全マニフェストが保持するエントリ数の合計を返す
     * @return エントリ数合計
     */
    qint64 globalEntries() const;

private:
    /**
     * @class ManifestRecord
     * @brief 計画本体と予算記録を束ねた内部保持単位
     *
     * manifest : 所有する復元計画本体
     * pathBytes : 本計画が保持するUTF-8 pathバイト数
     * ownerUid : 予算集計に使う呼び出し元UID
     */
    struct ManifestRecord {
        std::unique_ptr<RestoreManifest> manifest; // 所有する復元計画本体
        qint64 pathBytes = 0;                      // 本計画が保持するUTF-8 pathバイト数
        uint ownerUid = 0;                          // 予算集計に使う呼び出し元UID
    };

    using ManifestMap = std::map<QString, ManifestRecord>;

    /**
     * @brief owner一致とTTL有効を確認して計画を取り出す
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @param err 結果エラーの格納先
     * @return 所有確認済みの計画、拒否時はnullptr
     */
    RestoreManifest *findOwned(const QString &id, const QString &owner,
                               ManifestError *err);

    /**
     * @brief 任意のエラー格納先へ値を設定する
     * @param err 結果エラーの格納先 (空でもよい)
     * @param value 設定する値
     */
    static void setError(ManifestError *err, ManifestError value);

    /**
     * @brief 実時間clockの現在時刻を返す
     * @return 現在時刻 (ミリ秒)
     */
    static qint64 defaultNowMs();

    /**
     * @brief 指定UIDのマニフェストが保持するエントリ数とpathバイト数の合計を返す
     * @param ownerUid 集計対象UID
     * @param entriesOut エントリ数合計の格納先
     * @param pathBytesOut pathバイト数合計の格納先
     */
    void usageForUid(uint ownerUid, qint64 *entriesOut, qint64 *pathBytesOut) const;

    ManifestMap m_manifests;                          // 識別子を鍵とする計画と予算記録の所有map
    std::function<qint64()> m_clock;                  // TTL判定と活動更新に使う時刻源
    int m_maxEntriesPerManifest = kMaxEntriesPerManifest; // マニフェスト単位のエントリ数上限
    qint64 m_maxPathBytesPerManifest = kMaxPathBytesPerManifest; // マニフェスト単位のUTF-8 pathバイト数上限
    qint64 m_maxEntriesGlobal = kMaxEntriesGlobal;    // registry全体のエントリ数上限
    qint64 m_maxPathBytesGlobal = kMaxPathBytesGlobal; // registry全体のUTF-8 pathバイト数上限
    qint64 m_maxEntriesPerUid = kMaxEntriesPerUid;    // UID単位のエントリ数上限
    qint64 m_maxPathBytesPerUid = kMaxPathBytesPerUid; // UID単位のUTF-8 pathバイト数上限
};

/**
 * @class FinishedRestorePlanStore
 * @brief 終端した復元計画の状態を短時間だけowner束縛で保持する
 *
 * 終端signalはsystem busの全接続へ届くため、失敗したpathなどの詳細は載せない
 * 詳細はこのstoreに残し、計画のownerだけが [GetRestorePlanStatus] で取得する
 *
 * 保持期間と件数に上限を設け、上限を超えた場合は期限の近いものから破棄する
 * 単一Qtイベントループスレッドから利用する非スレッドセーフなコンテナである
 *
 * m_records : 識別子を鍵とする終端記録の所有map
 * m_clock : 保持期限の判定に使う時刻源 (差し替え可能)
 */
class FinishedRestorePlanStore
{
public:
    static constexpr qint64 kDefaultRetentionMs = 60 * 1000; // 既定の保持期間 (ミリ秒)
    static constexpr int kMaxEntries = RestoreManifestRegistry::kMaxManifestsGlobal; // 最大保持件数

    /**
     * @brief 実時間clockを使用する空のstoreを構築する
     */
    FinishedRestorePlanStore();

    /**
     * @brief 保持期限の判定に使うclockを差し替える
     * @param clock ミリ秒を返す関数
     *              空なら実時間clockへ戻す
     */
    void setClock(std::function<qint64()> clock);

    /**
     * @brief 終端した計画の状態を記録する
     * @param owner 計画のowner (D-Busのユニーク名)
     * @param status 終端状態を設定済みの計画状態
     *
     * idが空の場合は記録しない
     * ownerの照合はRestoreManifestRegistryと同じく完全一致で行う
     */
    void record(const QString &owner, const ManifestStatus &status);

    /**
     * @brief owner確認後に終端した計画の状態を取得する
     * @param id マニフェスト識別子
     * @param owner 呼び出し元owner
     * @return 保持期限内かつownerが一致する場合は状態、それ以外はstd::nullopt
     */
    std::optional<ManifestStatus> status(const QString &id, const QString &owner);

    /**
     * @brief 消失したownerに属する記録を全て削除する
     * @param owner 削除対象owner
     * @return 削除した記録数
     */
    int removeByOwner(const QString &owner);

    /**
     * @brief 保持期限を過ぎた記録を削除する
     * @return 削除した記録数
     */
    int purgeExpired();

    /**
     * @brief 保持している記録数を返す
     * @return 記録数
     */
    int count() const;

    /**
     * @brief ownerに属する記録数を返す
     * @param owner 対象owner
     * @return 記録数
     */
    int countForOwner(const QString &owner) const;

private:
    /**
     * @class FinishedRecord
     * @brief 終端記録1件の内部保持単位
     *
     * owner : 計画のowner (D-Busのユニーク名)
     * status : 終端状態を設定済みの計画状態
     * expiresAtMs : 破棄する保持期限 (ミリ秒)
     */
    struct FinishedRecord {
        QString owner;          // 計画のowner
        ManifestStatus status;  // 終端状態を設定済みの計画状態
        qint64 expiresAtMs = 0; // 破棄する保持期限 (ミリ秒)
    };

    std::map<QString, FinishedRecord> m_records; // 識別子を鍵とする終端記録の所有map
    std::function<qint64()> m_clock;             // 保持期限の判定に使う時刻源
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREMANIFESTREGISTRY_H
