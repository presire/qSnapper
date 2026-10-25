/**
 * @file restoremanifesttypes.h
 * @brief 復元マニフェストが扱う素朴なデータ型と列挙体の定義
 *
 * 本ヘッダは副作用を持たない型だけを集めたものである
 * 状態機械の本体は restoremanifest.h の RestoreManifest を参照すること
 * 所有と上限の管理は restoremanifestregistry.h の RestoreManifestRegistry を参照すること
 */

#ifndef QSNAPPER_RESTOREMANIFESTTYPES_H
#define QSNAPPER_RESTOREMANIFESTTYPES_H

#include <QString>
#include <QStringList>
#include <QVector>

// 復元計画の状態と転送データを扱う名前空間
namespace qsnapper::restore {

/**
 * @class ManifestState
 * @brief 復元マニフェストの単調なライフサイクル状態
 *
 * 状態は Staging から始まり一方向にだけ進む
 * 終端状態 (Completed / Failed / Cancelled) へ達した計画は再利用しない
 */
enum class ManifestState {
    Staging,    // エントリ受付中
    Frozen,     // 凍結済みで実行待ち
    Running,    // 実行中
    Completed,  // 正常に完了
    Failed,     // 失敗して停止
    Cancelled   // 取り消されて停止
};

/**
 * @class RestoreMode
 * @brief マニフェストに固定される復元方式
 *
 * 計画の作成時に確定し後から変更しない
 * 実行段はこの値に従って適用手段を選ぶ
 */
enum class RestoreMode {
    YastCompatible, // YaST互換方式で復元する
    DirectCopy      // 直接複写で高速に復元する
};

/**
 * @class ManifestError
 * @brief マニフェスト操作の機械可読エラー
 *
 * D-Bus層はこの値を契約に従った応答へ写像する
 * 成功時は None を格納先へ設定する
 */
enum class ManifestError {
    None,             // 成功
    NotFound,         // 指定IDの計画が存在しない
    OwnerMismatch,    // 呼び出し元が所有者と一致しない
    WrongState,       // 現在の状態では操作できない
    Expired,          // TTL超過で破棄済み
    CapacityExceeded, // マニフェスト単位の上限を超過する
    InvalidArgument,  // 引数が不正である
    AlreadyTerminal,  // 終端済みで遷移できない
    GlobalLimit       // 全体予算の上限を超過する
};

/**
 * @class RestoreEntry
 * @brief 順序を保持する単一の復元対象
 *
 * path : 復元対象の絶対パス
 * changeType : クライアント契約の変更種別 (created / deleted / typechanged / modified)
 */
struct RestoreEntry {
    QString path;       // 復元対象の絶対パス
    QString changeType; // クライアント契約の変更種別
};

/**
 * @class RestoreEntryOutcome
 * @brief 単一エントリを適用した結果
 *
 * 復元できない種類 (FIFO・ソケット・デバイスファイル) を計画全体の失敗にせず、
 * 飛ばしたことを利用者へ伝えられるよう、成功と失敗のほかに「スキップ」を区別する
 */
enum class RestoreEntryOutcome {
    Applied,    // 適用した (削除を含む)
    Skipped,    // 復元できない種類のため、live側に触れずに飛ばした
    Failed      // 適用できなかった (計画はここで停止する)
};

/**
 * @class RestoreChunkTally
 * @brief 1つのchunkで処理したエントリの内訳
 *
 * cursorの前進と同時に記録し、cursorと内訳が食い違わないようにする
 *
 * deletedCount : live側から削除したエントリ数 (変更種別created)
 * skippedPaths : スキップしたエントリのパス (処理した順)
 */
struct RestoreChunkTally {
    int deletedCount = 0;       // live側から削除したエントリ数
    QStringList skippedPaths;   // スキップしたエントリのパス
};

/**
 * @class ManifestStatus
 * @brief 外部へ公開できるマニフェストの状態スナップショット
 *
 * 値の複写であり registry 内部の生の状態を露出させない
 * 復元はロールバックできないため、途中で失敗した場合に利用者が手作業で確認できるよう、
 * 削除・スキップの件数と、失敗したエントリの位置を終端状態にも残す
 *
 * id : 推測困難なマニフェスト識別子
 * state : 現在のライフサイクル状態
 * totalEntries : 保存済みエントリ総数
 * cursor : 次に処理する0始まりの位置
 * processed : 処理済み件数 (cursorと同値、スキップを含む)
 * mode : 構築時に固定された復元方式
 * configName : 構築時に固定されたSnapper設定名
 * snapshotNumber : 構築時に固定された復元元スナップショット番号
 * counterpartSnapshotNumber : 比較相手の番号 (0は現在のシステムを表す)
 * lastError : 人間向けの失敗理由 (失敗時以外は空)
 * deletedCount : live側から削除したエントリ数
 * skippedCount : スキップしたエントリ数
 * skippedPaths : スキップしたエントリのパス (先頭からkMaxRecordedSkippedPaths件まで)
 * failedIndex : 失敗したエントリの0始まりの位置 (失敗していない場合は-1)
 * failedPath : 失敗したエントリのパス (失敗していない場合は空)
 */
struct ManifestStatus {
    static constexpr int kMaxRecordedSkippedPaths = 100;   // 記録するスキップしたパスの上限 (状態の応答を肥大化させないため)

    QString id;                                  // 推測困難なマニフェスト識別子
    ManifestState state = ManifestState::Staging; // 現在のライフサイクル状態
    int totalEntries = 0;                        // 保存済みエントリ総数
    int cursor = 0;                              // 次に処理する0始まりの位置
    int processed = 0;                           // 処理済み件数
    RestoreMode mode = RestoreMode::YastCompatible; // 構築時に固定された復元方式
    QString configName;                          // 構築時に固定されたSnapper設定名
    int snapshotNumber = -1;                     // 構築時に固定された復元元番号
    int counterpartSnapshotNumber = -1;          // 比較相手の番号 (0は現在のシステム)
    QString lastError;                           // 人間向けの失敗理由
    int deletedCount = 0;                        // live側から削除したエントリ数
    int skippedCount = 0;                        // スキップしたエントリ数
    QStringList skippedPaths;                    // スキップしたエントリのパス (上限あり)
    int failedIndex = -1;                        // 失敗したエントリの位置 (無しは-1)
    QString failedPath;                          // 失敗したエントリのパス (無しは空)
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREMANIFESTTYPES_H
