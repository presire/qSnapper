/**
 * @file filechangemodel_p.h
 * @brief 変更レコード解析の内部ヘルパー
 *
 * setupModelDataとcollectChangedPathsとfilterChangeOutputByPathsで共有する
 * 行指向の変更レコード解析だけを集めた内部ヘッダーであり
 * 公開APIではないため外部からは取り込まない
 * inlineで定義し、filechangemodel_detail名前空間で外部リンケージとする
 */

#ifndef FILECHANGEMODEL_P_H
#define FILECHANGEMODEL_P_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QSet>
#include <QRegularExpression>
#include <QDebug>
#include "filechangeitem.h"

// 変更レコード解析の内部ヘルパーをまとめる名前空間
namespace filechangemodel_detail {

/**
 * @struct ChangeInfo
 * @brief 解析済みの変更レコードを保持する
 *
 * 1行の変更レコードを分解した結果であり、ツリー構築の中間表現として使う
 *
 * path        : 正規化済みの絶対パス
 * type        : 変更タイプ
 * statusFlags : 詳細ステータスフラグ
 * isDirectory : 末尾スラッシュ付きで報告されたディレクトリかどうか
 */
struct ChangeInfo
{
    QString path;                   // 正規化済みの絶対パス
    FileChangeItem::ChangeType type; // 変更タイプ
    QString statusFlags;            // 詳細ステータスフラグ
    bool isDirectory;               // 末尾スラッシュ付きで報告されたディレクトリかどうか
};

/**
 * @brief レガシーの変更レコードからステータスとパスを抽出する
 *
 * 最初の空白区切りだけを消費するため、パス内の空白はそのまま保持する
 * 末尾スラッシュは除去して格納し、ディレクトリ判定は別に保持する
 *
 * @param line 改行を除いた変更レコード
 * @param info 解析結果の格納先
 * @return 有効なレコードを解析できた場合はtrue
 */
inline bool parseChangeRecord(const QString &line, ChangeInfo *info)
{
    const int separatorStart = line.indexOf(QRegularExpression(QStringLiteral("\\s+")));
    if (separatorStart <= 0) {
        return false;
    }

    int pathStart = separatorStart;
    while (pathStart < line.size() && line.at(pathStart).isSpace()) {
        ++pathStart;
    }
    if (pathStart >= line.size()) {
        return false;
    }

    const QString filePath = line.mid(pathStart);
    info->statusFlags = line.left(separatorStart);
    info->path = filePath.endsWith('/') ? filePath.left(filePath.size() - 1) : filePath;
    info->isDirectory = filePath.endsWith('/');
    return !info->path.isEmpty();
}

/**
 * @brief パスの親ディレクトリを集合へ追加する
 *
 * 中間ディレクトリの合成ノード生成に使い、ルート直下まで全階層を登録する
 *
 * @param path 正規化済みの絶対パス
 * @param parentPaths 親パスを格納する集合
 */
inline void collectParentPaths(const QString &path, QSet<QString> *parentPaths)
{
    int separator = path.indexOf('/', 1);
    while (separator > 0) {
        parentPaths->insert(path.left(separator));
        separator = path.indexOf('/', separator + 1);
    }
}


} // namespace filechangemodel_detail

#endif // FILECHANGEMODEL_P_H
