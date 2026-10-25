#ifndef QSNAPPER_RESTOREVALIDATION_H
#define QSNAPPER_RESTOREVALIDATION_H

/**
 * @file restorevalidation.h
 * @brief 復元宛先の組み立てと凍結済みentryの検証を行う純粋関数群
 *
 * libsnapperとファイルシステムへの依存を呼び出し側へ隔離し、本ヘッダの関数はシステムコールを発行しない
 * 木の観測は呼び出し側が渡す関数で行う
 */

#include <QString>
#include <QtGlobal>

#include <functional>

#include "restoremanifest.h"

struct stat;

namespace qsnapper::restore {

/**
 * @brief 復元対象のパスを1つの木で観測した結果の種類
 *
 * 存在の判定はlibsnapperの比較と同じく、symlinkを辿らず、別のファイルシステムへ降りない木として行う
 * 中間成分が非ディレクトリ・symlink・別のファイルシステム (マウントポイントや別のサブボリューム) の場合、そのパスは木に含まれないためAbsentとする
 */
enum class RestorePathPresence {
    Present,  // パスが実在する
    Absent,   // パスが木に含まれない
    Unknown   // 入出力エラー等で存在を確定できなかった
};

/**
 * @brief 復元対象のパスを1つの木で観測した状態
 *
 * 変更の有無の判定に使うstatの項目だけを、libsnapperに依存しない形で保持する
 * btrfsのスナップショットは元のinode番号とctimeを引き継ぐため、変更されていないパスはinode番号とctimeを含む全項目が一致する
 */
struct RestorePathObservation {
    RestorePathPresence presence = RestorePathPresence::Unknown; // 観測結果の種類
    quint32 fileType = 0;                                        // st_modeのファイル種別 (S_IFMTの範囲)
    quint32 mode = 0;                                            // st_modeの権限ビット
    quint64 inode = 0;                                           // inode番号
    quint32 uid = 0;                                             // 所有者
    quint32 gid = 0;                                             // グループ
    qint64 size = 0;                                             // サイズ (バイト)
    qint64 mtimeSec = 0;                                         // 内容の更新時刻 (秒)
    qint64 mtimeNsec = 0;                                        // 内容の更新時刻 (ナノ秒)
    qint64 ctimeSec = 0;                                         // 属性の変更時刻 (秒)
    qint64 ctimeNsec = 0;                                        // 属性の変更時刻 (ナノ秒)
};

/**
 * @brief 復元対象のconfig相対パスを1つの木で観測する関数の型
 *
 * 引数はbuildRestoreDestination()が返す相対パスであり、実行段が操作するパスと同じものである
 */
using RestorePathObserver = std::function<RestorePathObservation(const QString &relativePath)>;

/**
 * @brief lstat相当の結果を観測結果へ変換する
 * @param info symlinkを辿らずに取得したstat
 * @return presenceをPresentにした観測結果
 */
RestorePathObservation restorePathObservationFromStat(const struct stat &info);

/**
 * @brief 凍結済みentry 1件が、復元元と比較相手の木の実際の差分に裏付けられているか判定する
 *
 * libsnapperの比較 (復元元をsnapshot1、比較相手をsnapshot2とする向き) と同じ意味で、次の場合だけtrueを返す
 *   created     : 復元元に無く、比較相手に有る (実行段はlive側を削除する)
 *   deleted     : 復元元に有り、比較相手に無い (実行段は復元元からコピーする)
 *   typechanged : 両方に有り、ファイル種別が異なる
 *   modified    : 両方に有り、ファイル種別が同じで、inode・権限・所有者・グループ・サイズ・mtime・ctimeのいずれかが異なる
 * どちらかの観測がUnknownの場合と、未知のchangeTypeはfalseとする
 *
 * modifiedの判定は、内容や属性の変更でctimeが必ず更新されることに依拠する
 * 差分の無いパスを変更ありと偽った計画は、スナップショットの内容でlive側を上書きさせる攻撃になるため拒否する
 *
 * @param changeType 凍結済みentryの変更種別
 * @param source 復元元の木での観測結果
 * @param counterpart 比較相手の木での観測結果
 * @return entryが実際の差分に裏付けられている場合true
 */
bool isRestoreEntryBackedByTrees(const QString &changeType,
                                 const RestorePathObservation &source,
                                 const RestorePathObservation &counterpart);

/**
 * @brief 凍結済みの全entryを、復元元と比較相手の木の直接観測で検証する
 *
 * libsnapperの比較結果 (snapper::Comparison) は照合の基準に使わない
 * read-only同士の比較では、libsnapperが保存するfilelistキャッシュが読まれる
 * このキャッシュはstatusと名前と改行を空白区切りで並べた行指向の形式で、名前をエスケープしない
 * 改行を含むファイル名を置かれると、キャッシュの再読込時に攻撃者が選んだパスが偽のentryとして比較結果へ混入するため、比較結果を基準にすると偽のentryを権威として受理してしまう
 * 代わりに、認可時に固定した復元元のdirfdと比較相手の木から、entryごとに存在と種別を直接観測して判定する
 *
 * entryはkEntriesPerValidationChunk件ずつの部分列で読み、凍結済み計画全体を一度にメモリへ展開しない
 * 1件でも裏付けのないentryがあれば計画全体を失敗させる (fail-closed)
 *
 * @param registry 凍結済み計画を保持するregistry
 * @param manifestId ownerに束縛されたマニフェストID
 * @param owner 呼び出し元owner
 * @param subvolume commit時に固定したconfigのSUBVOLUME
 * @param observeSource 復元元の木を観測する関数
 * @param observeCounterpart 比較相手の木を観測する関数
 * @param err 結果エラーの格納先 (省略可)
 * @return 全entryが実際の差分に裏付けられている場合true
 */
bool validateFrozenEntriesAgainstTrees(RestoreManifestRegistry &registry,
                                       const QString &manifestId,
                                       const QString &owner,
                                       const QString &subvolume,
                                       const RestorePathObserver &observeSource,
                                       const RestorePathObserver &observeCounterpart,
                                       ManifestError *err);

/**
 * @brief config相対の復元対象名がスナップショット管理領域 (.snapshots) を指すか判定する
 *
 * 文字列の前方一致ではなく、空成分を除いた先頭成分で判定する
 * これにより"//.snapshots/..."のような空成分を挟んだ表記による迂回を防ぐ
 *
 * @param configRelativeName libsnapper File::getName()由来のconfig相対名
 * @return 先頭成分が".snapshots"の場合true
 */
bool isSnapshotMetadataRestoreName(const QString &configRelativeName);

/**
 * @brief 復元対象パスを重複排除用の正規化キーへ変換する
 *
 * 空成分を除き、成分を"/"で繋ぎ直した絶対パスを返す
 * 同じ実体を指す"/a"、"//a"、"/a/"が同じキーになるため、同じ計画内の異なるchunkの間でも重複を検出できる
 * 実行時の復元先も同じ成分列から組み立てるため、キーが一致するパスは同じ場所への操作になる
 *
 * @param path 変換対象の絶対パス
 * @param keyOut 正規化キー (rootサブボリューム直下なら"/")
 * @return 先頭が"/"で、"." / ".."成分・制御文字を含まない場合true
 */
bool canonicalizeRestorePlanPathKey(const QString &path, QString *keyOut);

/**
 * @brief configのSUBVOLUMEを空成分を除いた絶対パスへ正規化する
 * @param subvolume configのSUBVOLUME
 * @param rootPathOut 正規化結果 (rootサブボリュームなら"/")
 * @return 絶対パスで、"." / ".."成分・制御文字を含まない場合true
 */
bool normalizeRestoreSubvolume(const QString &subvolume, QString *rootPathOut);

/**
 * @brief config相対の復元対象名とconfigのSUBVOLUMEから、live側の宛先を組み立てる
 *
 * GetFileChanges*が返し、クライアントがStageする名前はconfig相対名 (例: SUBVOLUME=/homeでは"/alice/file") である
 * live側の宛先は"<SUBVOLUME>/<name>"であり、"/"基準で解決してはならない
 *
 * subvolume / 名前のいずれも、空成分を除いて正規化する
 * "." / ".."成分、制御文字、空の名前、".snapshots"配下の名前は拒否する
 *
 * @param subvolume configのSUBVOLUME (絶対パス)
 * @param configRelativeName config相対の復元対象名 (絶対パス表記)
 * @param rootPathOut 正規化したSUBVOLUME (*BeneathRoot系helperのrootPathに渡す)
 * @param destinationOut live側の宛先絶対パス (rootPathOut配下)
 * @param relativeOut rootPathOut (およびスナップショット dir) からの相対パス
 * @return 組み立てに成功した場合true
 */
bool buildRestoreDestination(const QString &subvolume,
                             const QString &configRelativeName,
                             QString *rootPathOut,
                             QString *destinationOut,
                             QString *relativeOut);

/**
 * @brief 復元後にrootサブボリュームをrwへ戻す安全ネットの実行条件
 */
struct RootReadWriteSafetyNetState {
    bool executionStarted = false;          // 認可済みで実行を開始した計画か
    bool targetsRootSubvolume = false;      // 対象configのSUBVOLUMEが"/"か
    bool preRestoreStateKnown = false;      // 復元前のread-only状態をcommit時に記録できたか
    bool preRestoreReadOnly = true;         // 復元前のrootサブボリュームがread-onlyだったか
};

/**
 * @brief 復元後にrootサブボリュームをrwへ戻してよいか判定する
 *
 * 次の全てを満たす場合のみtrueを返す
 *   - 認可済みで実際に実行を開始した計画である
 *   - 対象configのSUBVOLUMEが"/"である
 *   - commit時に記録した復元前の状態がrwであり、復元後にread-onlyになっている
 * 1つでも判定できない場合はfalse (特権操作を行わない) とする
 *
 * @param state commit時に記録した状態
 * @param readOnlyAfterRestore 復元後に観測したrootサブボリュームのread-only状態
 * @return rwへ戻すべき場合true
 */
bool shouldRestoreRootReadWrite(const RootReadWriteSafetyNetState &state,
                                bool readOnlyAfterRestore);

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREVALIDATION_H
