/**
 * @file filechangeitem.cpp
 * @brief FileChangeItemの実装
 *
 * ツリーの1ノードとしての生成と破棄と子管理と属性取得を行う
 */

#include "filechangemodel.h"
#include <QFileInfo>
#include <QtAlgorithms>


// ============================================================================
// FileChangeItem Implementation
// ============================================================================

/**
 * @brief FileChangeItemのコンストラクタ
 *
 * ファイル変更アイテムを作成し、パス、変更タイプ、親アイテムを設定する
 *
 * @param path ファイルパス
 * @param type 変更タイプ (Created, Deleted, Modifiedなど)
 * @param parent 親アイテムへのポインタ
 */
FileChangeItem::FileChangeItem(const QString &path, ChangeType type, const QString &statusFlags, FileChangeItem *parent)
    : m_path(path), m_changeType(type), m_statusFlags(statusFlags), m_parent(parent)
{
}

/**
 * @brief FileChangeItemのデストラクタ
 *
 * 全ての子アイテムを削除する
 */
FileChangeItem::~FileChangeItem()
{
    qDeleteAll(m_children);
}

/**
 * @brief 子アイテムを追加
 *
 * このアイテムに子アイテムを追加する
 *
 * @param child 追加する子アイテムへのポインタ
 */
void FileChangeItem::appendChild(FileChangeItem *child)
{
    m_children.append(child);
}

/**
 * @brief 指定された行番号の子アイテムを取得
 *
 * 指定された行番号に対応する子アイテムを返す
 *
 * @param row 子アイテムの行番号
 * @return 子アイテムへのポインタ (範囲外の場合はnullptr)
 */
FileChangeItem *FileChangeItem::child(int row)
{
    if (row < 0 || row >= m_children.size())
        return nullptr;
    return m_children.at(row);
}

/**
 * @brief 子アイテムの数を取得
 *
 * このアイテムが持つ子アイテムの数を返す
 *
 * @return 子アイテムの数
 */
int FileChangeItem::childCount() const
{
    return m_children.size();
}

/**
 * @brief 親アイテム内での行番号を取得
 *
 * このアイテムが親アイテムの何番目の子であるかを返す
 *
 * @return 行番号 (親がない場合は0)
 */
int FileChangeItem::row() const
{
    if (m_parent)
        return m_parent->m_children.indexOf(const_cast<FileChangeItem*>(this));
    return 0;
}

/**
 * @brief 親アイテムを取得
 *
 * このアイテムの親アイテムへのポインタを返す
 *
 * @return 親アイテムへのポインタ
 */
FileChangeItem *FileChangeItem::parent()
{
    return m_parent;
}

/**
 * @brief ファイル名またはディレクトリ名を取得
 *
 * パスからファイル名またはディレクトリ名を抽出して返す
 *
 * @return ファイル名またはディレクトリ名
 */
QString FileChangeItem::name() const
{
    if (m_path.isEmpty())
        return QString();

    // パスの末尾がスラッシュの場合は削除してから処理
    QString path = m_path;
    if (path.endsWith('/') && path.length() > 1) {
        path = path.left(path.length() - 1);
    }

    QFileInfo info(path);
    QString fileName = info.fileName();

    // ルートディレクトリの場合
    if (fileName.isEmpty() && path == "/") {
        return "/";
    }

    return fileName;
}

/**
 * @brief ディレクトリかどうかを判定
 *
 * パスの末尾がスラッシュで終わっているか、子要素があればディレクトリと判定する
 *
 * @return ディレクトリの場合: true、それ以外: false
 */
bool FileChangeItem::isDirectory() const
{
    // パスの末尾が/で終わっているか、子要素があればディレクトリ
    return m_path.endsWith('/') || !m_children.isEmpty();
}
