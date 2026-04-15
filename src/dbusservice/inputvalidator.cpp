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
 * @brief NUL / 改行 / キャリッジリターンを含むかを判定する補助関数
 *
 * QStringが内部でUTF-16を保持しているため、制御文字の埋め込みは正規表現よりQChar比較で確実に検出できる。
 */
bool containsDangerousChar(const QString &s)
{
    for (const QChar &c : s) {
        const ushort u = c.unicode();
        if (u == 0 || u == '\n' || u == '\r') {
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
 *   4. NUL / 改行チェック (belt and suspenders)
 *   5. allowlist正規表現
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
    if (containsDangerousChar(name)) {
        return false;
    }
    const QRegularExpressionMatch m = configNameRegex().match(name);
    return m.hasMatch();
}

/**
 * @brief inputvalidator.h の isPathWithinSnapshotRoot 実装
 *
 * weakly_canonicalを使わずに、lexically_normal + lexically_relativeで判定する。
 * weakly_canonical は FS にタッチし存在しないパスで挙動がぶれる。
 * シンボリックリンク解決は契約どおり呼び出し側 (openat+O_NOFOLLOW) の責務。
 *
 * sibling root trick ("/a/root" vs "/a/root-evil") は
 * lexically_relativeが "../root-evil/..." を返すため先頭 ".." チェックで自然に弾ける。
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
        // (lexically_normalは末尾スラッシュを残すことがあり、lexically_relativeの結果がぶれるため)
        QString rootTrim = snapshotRoot;
        while (rootTrim.size() > 1 && rootTrim.endsWith(QLatin1Char('/'))) {
            rootTrim.chop(1);
        }

        const fs::path p    = fs::path(filePath.toStdString()).lexically_normal();
        const fs::path root = fs::path(rootTrim.toStdString()).lexically_normal();

        const fs::path rel = p.lexically_relative(root);
        if (rel.empty()) {
            return false;
        }

        // 先頭コンポーネントが ".." ならrootを脱出している
        const auto it = rel.begin();
        if (it != rel.end() && *it == "..") {
            return false;
        }
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}

} // namespace security
} // namespace qsnapper
