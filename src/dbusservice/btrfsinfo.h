#ifndef QSNAPPER_BTRFSINFO_H
#define QSNAPPER_BTRFSINFO_H

/**
 * @file btrfsinfo.h
 * @brief Btrfsの使用量とサブボリューム一覧を読み取る読み取り専用API
 *
 * root D-Busヘルパーから呼ばれる想定であり、ファイルシステムを変更しない
 * openBtrfsDirectory()が返すfdに対象を固定し、以降の読み取りをパス文字列ではなく
 * そのfdだけで行うことで、検査した対象と読み取った対象が食い違わないようにする
 *
 * 対応する単体テスト: tests/unit/tst_btrfsinfo.cpp
 */

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace qsnapper::btrfs {

/**
 * @brief 読み取り結果の種別
 */
enum class InfoError {
    None,          // 読み取りに成功した
    NotBtrfs,      // 対象がBtrfsではなかった
    LimitExceeded, // 読み取る件数が上限を超えた
    Failed         // 上記以外の理由で読み取れなかった
};

/**
 * @brief BTRFS_IOC_SPACE_INFOが返す領域1件の使用量
 */
struct SpaceEntry {
    quint64 flags;      // 領域の種別とプロファイル、グローバル予約を示すビット
    quint64 totalBytes; // 領域の総容量
    quint64 usedBytes;  // 領域の使用量
};

/**
 * @brief BTRFS_IOC_DEV_INFOが返すデバイス1件の使用量
 */
struct DeviceEntry {
    quint64 devid;      // デバイスID
    quint64 totalBytes; // デバイスの総容量
    quint64 bytesUsed;  // デバイスの使用量
};

/**
 * @brief BTRFS_IOC_FS_INFOと各ioctlから集めたファイルシステム全体の情報
 *
 * spacesとdevicesは部分的な結果を成功として返さない
 * 取得数がカーネルの申告と一致しない場合はInfoError::Failedになる
 */
struct FilesystemUsage {
    quint64 numDevices = 0;        // カーネルが申告するデバイス数
    quint32 nodesize = 0;          // メタデータのノードサイズ
    quint32 sectorsize = 0;        // セクターサイズ
    QVector<SpaceEntry> spaces;    // 領域ごとの使用量
    QVector<DeviceEntry> devices;  // デバイスごとの使用量
};

/**
 * @brief サブボリューム1件の情報
 */
struct SubvolumeEntry {
    quint64 id = 0;       // サブボリュームID
    quint64 parentId = 0; // 親サブボリュームID
    qint64 otimeSec = 0;  // 作成時刻 (エポック秒)
    bool readOnly = false; // 読み取り専用で作成されたか
    QByteArray path;      // FSツリーを基点とした相対パス
};

/**
 * @brief サブボリューム一覧の列挙に掛ける上限
 *
 * 一覧はroot D-Busヘルパーの応答へ載るため、件数・パス長・所要時間のいずれでも打ち切る
 * 打ち切った場合は呼び出し側へtruncatedとして通知し、完全な一覧と取り違えないようにする
 */
struct SubvolumeListLimits {
    int maxEntries = 10000;                   // 列挙する最大件数
    qint64 maxPathBytes = 2 * 1024 * 1024;    // パス長の累計上限
    int maxMilliseconds = 1500;               // 列挙に掛ける最大時間
};

// SPACE_INFOで受け付ける領域数の上限
constexpr int MaxSpaceSlots = 64;
// DEV_INFOで問い合わせるdevidの上限
constexpr quint64 MaxDeviceId = 64;

/**
 * @brief 絶対パスをBtrfsディレクトリとして開き、以降の操作に使うfdを返す
 *
 * openat2()にO_NOFOLLOWとRESOLVE_NO_SYMLINKS、RESOLVE_NO_MAGICLINKSを指定し、
 * 途中の成分を含めてsymlinkを解決しない
 * マウント境界は許可するためRESOLVE_NO_XDEVは指定しない
 * 開いた後にfstat()でディレクトリであること、fstatfs()でBtrfsであることを確認する
 * openat2()がENOSYSを返すカーネルでは代替の解決手段を使わずに失敗する
 * 成分単位のopenat()へ切り替えると、検証方針を同じ強度で維持できないためである
 *
 * @param absolutePath 開く対象の絶対パス
 * @return 成功時は呼び出し側がclose()すべきfd、失敗時は-1
 */
int openBtrfsDirectory(const QString &absolutePath);

/**
 * @brief 開いたBtrfsディレクトリfdからファイルシステム全体の使用量を読み取る
 *
 * 対象はfdだけで特定し、パス文字列は使わない
 * デバイス数がカーネルの申告と一致しない場合や、領域数が取得中に増えた場合は
 * 部分的な合計を返さずに失敗する
 *
 * @param dirFd openBtrfsDirectory()が返したBtrfsディレクトリのfd
 * @param out 読み取り結果の格納先
 * @return 成功時はInfoError::None、対象がBtrfsでなければInfoError::NotBtrfs
 * @note 上限を超えた場合はInfoError::LimitExceededを返し、outは変更しない
 */
InfoError readFilesystemUsage(int dirFd, FilesystemUsage *out);

/**
 * @brief 開いたBtrfsディレクトリfdからサブボリュームを列挙する
 *
 * libbtrfsutilのfd版イテレータを使い、FSツリー (ID 5) を基点として列挙する
 * 上限に達した場合はそこで列挙を止め、truncatedをtrueにして成功を返す
 * 途中でエラーになった場合はoutを空にし、部分的な一覧を成功として返さない
 *
 * @param dirFd openBtrfsDirectory()が返したBtrfsディレクトリのfd
 * @param limits 列挙に掛ける上限
 * @param out 列挙結果の格納先
 * @param truncated 上限で打ち切った場合にtrueが入る
 * @return 成功時はInfoError::None、それ以外はInfoError::Failed
 */
InfoError listSubvolumes(int dirFd, const SubvolumeListLimits &limits,
                         QVector<SubvolumeEntry> *out, bool *truncated);

/**
 * @brief snapperのスナップショット本体を指すパスかどうかを判定する
 *
 * 判定するのは .snapshots/<10進数字>/snapshot という成分列がパスの末尾に現れる場合だけである
 * 文字列の末尾一致で判定すると .snapshots/12/snapshot-backup のような別名を誤って受理するため、
 * パスを成分へ分解して比較する
 *
 * @param path サブボリュームの相対パス
 * @return スナップショット本体を指す場合true
 */
bool isSnapperSnapshotPath(const QByteArray &path);

/**
 * @brief ファイルシステム使用量をCSVへ整形する
 *
 * 先頭は列見出し、続いてファイルシステム全体の情報を1行、領域とデバイスを各1行ずつ並べる
 * 数値は全て10進表記とする
 *
 * @param usage 整形する使用量
 * @return 改行で連結したCSV (末尾に改行は付けない)
 */
QString formatFilesystemUsageCsv(const FilesystemUsage &usage);

/**
 * @brief サブボリューム一覧をCSVへ整形する
 *
 * 先頭行で打ち切りの有無を示し、2行目を列見出しとする
 * パスは表示専用の値であり、識別子としては使わない
 *
 * @param entries 整形するサブボリューム一覧
 * @param truncated 上限で打ち切られた場合true
 * @return 改行で連結したCSV (末尾に改行は付けない)
 */
QString formatSubvolumeListCsv(const QVector<SubvolumeEntry> &entries, bool truncated);

} // namespace qsnapper::btrfs

#endif // QSNAPPER_BTRFSINFO_H
