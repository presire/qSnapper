/**
 * @file inputvalidator.cpp
 * @brief inputvalidator.h宣言の実装
 *
 * 本ファイルは、FSへのアクセスを行わない純粋関数のみを含む
 * テスト: tests/unit/tst_configname.cpp, tests/unit/tst_filepaths.cpp
 */

#include <QRegularExpression>
#include <QString>
#include <filesystem>
#include <limits.h>
#include "inputvalidator.h"

namespace qsnapper {
namespace security {

namespace {

/**
 * @brief configName許容文字の正規表現 (ASCII-only allowlist)
 *
 * プログラム開始時に1度だけコンパイルされ、以降の呼び出しで使い回される。
 * QRegularExpressionは、thread-safeなconst操作のみを行えば再入可能。
 */
const QRegularExpression &configNameRegex()
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return re;
}

/**
 * @brief 制御文字 (非印字文字) を含むかを判定する補助関数
 *
 * 拒否対象は以下の Unicode 範囲:
 *   - C0 制御文字  : U+0000 .. U+001F (NUL/HT/LF/CR/ESC 等を含む)
 *   - DEL          : U+007F
 *   - C1 制御文字  : U+0080 .. U+009F
 *
 * QStringが内部でUTF-16を保持しているため、制御文字の埋め込みは正規表現よりQChar比較で確実に検出できる。
 * ファイルパスやconfig名にこれらが正当に含まれることはない。
 */
bool containsDangerousChar(const QString &s)
{
    for (const QChar &c : s) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * @brief inputvalidator.hのvalidateConfigName実装
 *
 * ロジック順序:
 *   1. 空文字 / 長さ >255 を棄却
 *   2. "." / ".." を棄却 (regexでは弾けない)
 *   3. 先頭 '-' を棄却 (CLIオプション誤認防止)
 *   4. allowlist正規表現 (制御文字も自然に拒否される)
 *
 * 注: 制御文字 (containsDangerousChar 相当) は allowlist 正規表現 ^[A-Za-z0-9_.-]+$ で
 *     既に拒否されるため、明示チェックは不要。
 */
bool validateConfigName(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    if (name.size() > 255) {
        return false;
    }
    if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return false;
    }
    if (name.startsWith(QLatin1Char('-'))) {
        return false;
    }
    const QRegularExpressionMatch m = configNameRegex().match(name);
    return m.hasMatch();
}

/**
 * @brief inputvalidator.h の isPathWithinSnapshotRoot 実装
 *
 * weakly_canonicalを使わずに、lexically_normal で正規化した上で
 * 文字列プレフィクス比較＋パス区切り境界チェックで判定する。
 * weakly_canonical は FS にタッチし存在しないパスで挙動がぶれる。
 * シンボリックリンク解決は契約どおり呼び出し側 (openat+O_NOFOLLOW) の責務。
 *
 * 単純な find()==0 では sibling root trick ("/a/root" vs "/a/root-evil") を許容してしまうため、
 * プレフィクス一致後に「完全一致」または「直後が '/' 」を要求する。
 */
bool isPathWithinSnapshotRoot(const QString &filePath, const QString &snapshotRoot)
{
    if (filePath.isEmpty() || snapshotRoot.isEmpty()) {
        return false;
    }
    if (containsDangerousChar(filePath) || containsDangerousChar(snapshotRoot)) {
        return false;
    }
    if (!filePath.startsWith(QLatin1Char('/'))) {
        return false;
    }
    if (!snapshotRoot.startsWith(QLatin1Char('/'))) {
        return false;
    }
    // PATH_MAX を超えるパスは FS 側でもエラーになるので即時棄却
    if (filePath.size() > PATH_MAX - 1) {
        return false;
    }

    try {
        namespace fs = std::filesystem;

        // 末尾の余分な '/' を落としてから正規化
        // (lexically_normalは末尾スラッシュを残すことがあり、文字列比較の結果がぶれるため)
        QString rootTrim = snapshotRoot;
        while (rootTrim.size() > 1 && rootTrim.endsWith(QLatin1Char('/'))) {
            rootTrim.chop(1);
        }

        const fs::path p    = fs::path(filePath.toStdString()).lexically_normal();
        const fs::path root = fs::path(rootTrim.toStdString()).lexically_normal();

        const std::string ps = p.string();
        const std::string rs = root.string();

        // (a) 長さが root 未満ならプレフィクス成立不可
        if (ps.size() < rs.size()) {
            return false;
        }
        // (b) 先頭がroot と一致しなければ拒否
        if (ps.compare(0, rs.size(), rs) != 0) {
            return false;
        }
        // (c) 完全一致 (=root自体)、または root 直後がパス区切り '/' (= 子要素) のみ許可
        //     これがないと "/a/root" と "/a/root-evil" を取り違える
        return ps.size() == rs.size() || ps[rs.size()] == '/';
    }
    catch (const std::exception &) {
        return false;
    }
}

} // namespace security
} // namespace qsnapper
