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
 *   - 許容文字は [A-Za-z0-9_.-] のみ
 *   - 空文字列は拒否
 *   - 先頭が '-' の文字列は拒否 (snapper CLI のオプション誤認防止)
 *   - "." および ".." は完全一致で拒否 (パス要素として解釈されるため)
 *   - NUL / 改行 / 空白などの制御文字は上記allowlistから自然に除外される
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
 *   - snapshotRootが空文字列なら拒否 (万能一致の防止)
 *   - filePath / snapshotRoot に NUL または改行を含む場合は拒否
 *   - filePath の長さが PATH_MAX (4096) を超える場合は拒否
 *   - std::filesystem::path::lexically_normal()で正規化し、snapshotRootからの相対パスが ".." で始まらないことを確認する
 *   - シンボリックリンクの解決は呼び出し側で openat(O_NOFOLLOW) にて行う
 *     (この関数ではFSにタッチしない)
 *
 * @param filePath      検査対象の絶対パス
 * @param snapshotRoot  許可する基底ディレクトリ (絶対パス)
 * @return 基底内に収まっている場合: true、そうでなければ: false
 */
bool isPathWithinSnapshotRoot(const QString &filePath, const QString &snapshotRoot);

} // namespace security
} // namespace qsnapper

#endif // QSNAPPER_INPUTVALIDATOR_H
