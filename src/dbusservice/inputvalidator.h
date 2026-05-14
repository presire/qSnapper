#ifndef QSNAPPER_INPUTVALIDATOR_H
#define QSNAPPER_INPUTVALIDATOR_H

/**
 * @file inputvalidator.h
 * @brief D-Bus 経由で受け取った信頼できない入力を検証する純粋関数群
 *
 * 本ヘッダに宣言された関数は副作用を持たず、呼び出し側はD-Busスロットの先頭 (checkAuthorization より前) で入力を検査し、
 * 不正ならば即時拒否する。
 *
 * 対応する単体テスト:
 *   - tests/unit/tst_configname.cpp
 *   - tests/unit/tst_filepaths.cpp
 */

#include <QString>

namespace qsnapper {
namespace security {

/**
 * @brief Snapper設定名が安全かどうかを判定する
 *
 * 仕様:
 *   - 許容文字は [A-Za-z0-9_.-] のみ (ASCII allowlist)
 *   - 空文字列は拒否
 *   - 先頭が '-' の文字列は拒否 (snapper CLI のオプション誤認防止)
 *   - "." および ".." は完全一致で拒否 (パス要素として解釈されるため)
 *   - 制御文字 (Unicode C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) と
 *     空白・非ASCII文字は上記allowlistから自然に除外される
 *   - 長さ上限は 255 (NAME_MAX準拠)
 *
 * @param name D-Busから受け取った設定名
 * @return 安全な場合: true、拒否すべき場合: false
 */
bool validateConfigName(const QString &name);

/**
 * @brief 絶対パスfilePathがsnapshotRoot配下に収まっているかを判定する
 *
 * 仕様:
 *   - filePathが絶対パス (先頭 '/') でなければ拒否
 *   - snapshotRootが絶対パスでなければ拒否
 *   - snapshotRootが空文字列なら拒否 (万能一致の防止)
 *   - filePath / snapshotRoot のいずれかに制御文字
 *     (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含む場合は拒否
 *   - filePath の長さが PATH_MAX (4096) を超える場合は拒否
 *   - std::filesystem::path::lexically_normal() で正規化した上で、
 *     filePath の文字列が snapshotRoot を完全プレフィクスとして持ち、
 *     かつ境界が「完全一致」または「直後がパス区切り '/'」であることを要求する。
 *     単純な find()==0 では sibling root trick
 *     (例: root="/a/b/snapshot" / p="/a/b/snapshot-evil/...") を取り逃すため、
 *     '/' 境界チェックを必須としている
 *   - シンボリックリンクの解決は呼び出し側で openat(O_NOFOLLOW) にて行う
 *     (この関数ではFSにタッチしない)
 *
 * @note snapshotRoot は実用上 "/.snapshots/N/snapshot" 形式を想定している。
 *       単一スラッシュ "/" を渡した場合は子要素が境界チェックで弾かれるため
 *       対象外と扱う。
 *
 * @param filePath      検査対象の絶対パス
 * @param snapshotRoot  許可する基底ディレクトリ (絶対パス)
 * @return 基底内に収まっている場合: true、そうでなければ: false
 */
bool isPathWithinSnapshotRoot(const QString &filePath, const QString &snapshotRoot);

} // namespace security
} // namespace qsnapper

#endif // QSNAPPER_INPUTVALIDATOR_H
