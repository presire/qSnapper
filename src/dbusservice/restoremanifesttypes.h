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
 * @class ManifestStatus
 * @brief 外部へ公開できるマニフェストの状態スナップショット
 *
 * 値の複写であり registry 内部の生の状態を露出させない
 *
 * id : 推測困難なマニフェスト識別子
 * state : 現在のライフサイクル状態
 * totalEntries : 保存済みエントリ総数
 * cursor : 次に処理する0始まりの位置
 * processed : 処理済み件数 (cursorと同値)
 * mode : 構築時に固定された復元方式
 * configName : 構築時に固定されたSnapper設定名
 * snapshotNumber : 構築時に固定された復元元スナップショット番号
 * counterpartSnapshotNumber : 比較相手の番号 (0は現在のシステムを表す)
 * lastError : 人間向けの失敗理由 (失敗時以外は空)
 */
struct ManifestStatus {
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
};

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREMANIFESTTYPES_H
