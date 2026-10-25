#ifndef QSNAPPER_USERDATAPARSER_H
#define QSNAPPER_USERDATAPARSER_H

/**
 * @file userdataparser.h
 * @brief スナップショットのユーザデータ入力(key=valueを1行1組で並べた文字列)を解析する
 *
 * 作成ダイアログと編集ダイアログの入力欄は、同じ規則で解析する
 * 画面ごとにJavaScriptで解析すると、規則の食い違いで値が黙って書き換わるため、C++の1か所に集約する
 *
 * 利用する側 : SnapshotListModel::parseUserdata()と、その結果を表示するQMLの作成・編集ダイアログ
 * 依存 : Qt Core (QString、QStringList、QVariantMap)
 */

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace qsnapper {
namespace userdata {

/**
 * @enum ParseError
 * @brief ユーザデータの解析エラーの種類
 */
enum class ParseError {
    None,               // エラーなし
    MissingSeparator,   // 空行ではないのに=を含まない行がある
    EmptyKey,           // =の左側(キー)が空である
    DuplicateKey        // 同じキーが複数回指定されている
};

/**
 * @struct ParseResult
 * @brief ユーザデータの解析結果
 */
struct ParseResult {
    bool ok = true;                       // 解析に成功したかどうか
    QVariantMap values;                   // 解析できたキーと値 (ok == falseの場合は空)
    ParseError error = ParseError::None;  // エラーの種類
    int line = 0;                         // エラーを検出した行の番号 (1始まり、エラーなしの場合は0)
    QString key;                          // 重複したキー (DuplicateKeyの場合だけ設定する)
};

/**
 * @brief ユーザデータの入力文字列をキーと値の組へ分解する
 * @param text 改行区切りでkey=valueを並べた文字列
 * @return 解析結果、エラーがある場合はokがfalseでvaluesは空
 * @note 区切りは改行だけである
 *       カンマを区切りにすると、値にカンマを含む既存のユーザデータを編集して保存したときに、値が黙って分割されて失われるため、区切りにしない
 *       値は=の右側をそのまま保持し、前後の空白も削除しない
 *       値の前後の空白を削除すると、編集のたびに保存済みの値が変わってしまうためである
 *       キーは、行頭のインデントや=の前の空白が見た目の整形であるため、前後の空白だけを削除する
 *       qsnapper-dbus-serviceは、キーの前後の空白を禁止していない
 *       そのため、空白だけが異なるキーは削除後に衝突するため、重複キーとして拒否する
 *       空白だけの行は無視し、行末の改行コード (CR) は取り除く
 *       同じキーが複数回あると後の値で黙って上書きされるため、先に見つけた時点でエラーにする
 */
inline ParseResult parse(const QString &text)
{
    ParseResult result;
    QVariantMap values;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.trimmed().isEmpty()) {
            continue;
        }

        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator < 0) {
            result.ok = false;
            result.error = ParseError::MissingSeparator;
            result.line = i + 1;
            return result;
        }

        const QString key = line.left(separator).trimmed();
        if (key.isEmpty()) {
            result.ok = false;
            result.error = ParseError::EmptyKey;
            result.line = i + 1;
            return result;
        }

        if (values.contains(key)) {
            result.ok = false;
            result.error = ParseError::DuplicateKey;
            result.line = i + 1;
            result.key = key;
            return result;
        }

        values.insert(key, line.mid(separator + 1));
    }

    result.values = values;
    return result;
}

} // namespace userdata
} // namespace qsnapper

#endif // QSNAPPER_USERDATAPARSER_H
