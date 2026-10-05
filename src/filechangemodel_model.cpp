/**
 * @file filechangemodel_model.cpp
 * @brief 変更レコードからのツリー構築とモデルの実装と選択状態
 *
 * QAbstractItemModelの上書きとパスと変更タイプの解析
 * およびチェック状態の再帰操作を担当する
 */

#include "filechangemodel.h"
#include "filechangemodel_p.h"
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>


/**
 * @brief 指定された位置のインデックスを取得
 *
 * モデル内の指定された行、列、親インデックスに対応するQModelIndexを返す
 *
 * @param row 行番号
 * @param column 列番号
 * @param parent 親のQModelIndex
 * @return QModelIndex
 */
QModelIndex FileChangeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    FileChangeItem *parentItem = getItem(parent);
    FileChangeItem *childItem = parentItem->child(row);

    if (childItem) {
        return createIndex(row, column, childItem);
    }

    return QModelIndex();
}

/**
 * @brief 親のインデックスを取得
 *
 * 指定された子アイテムの親のQModelIndexを返す
 *
 * @param child 子のQModelIndex
 * @return 親のQModelIndex
 */
QModelIndex FileChangeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return QModelIndex();
    }

    FileChangeItem *childItem = getItem(child);
    FileChangeItem *parentItem = childItem->parent();

    if (parentItem == m_rootItem || parentItem == nullptr) {
        return QModelIndex();
    }

    return createIndex(parentItem->row(), 0, parentItem);
}

/**
 * @brief 行数を取得
 *
 * 指定された親アイテムの持つ子アイテムの数を返す
 *
 * @param parent 親のQModelIndex
 * @return 行数 (子アイテムの数)
 */
int FileChangeModel::rowCount(const QModelIndex &parent) const
{
    FileChangeItem *parentItem = getItem(parent);
    return parentItem->childCount();
}

/**
 * @brief 列数を取得
 *
 * このモデルは常に1列である
 *
 * @param parent 親のQModelIndex (未使用)
 * @return 列数 (常に1)
 */
int FileChangeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

/**
 * @brief データを取得
 *
 * 指定されたインデックスとロールに対応するデータを返す
 *
 * @param index データを取得したいQModelIndex
 * @param role データのロール (PathRole, NameRoleなど)
 * @return データのQVariant
 */
QVariant FileChangeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    FileChangeItem *item = getItem(index);

    switch (role) {
    case PathRole:
        return item->path();
    case NameRole:
        return item->name();
    case ChangeTypeRole:
        return item->changeType();
    case IsDirectoryRole:
        return item->isDirectory();
    case IsCheckedRole:
        return item->isChecked();
    case StatusFlagsRole:
        return item->statusFlags();
    case Qt::DisplayRole:
        return item->name();
    default:
        return QVariant();
    }
}

/**
 * @brief ロール名を取得
 *
 * QML等で使用するロール名のマッピングを返す
 *
 * @return ロール名のハッシュマップ
 */
QHash<int, QByteArray> FileChangeModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[PathRole] = "filePath";
    roles[NameRole] = "fileName";
    roles[ChangeTypeRole] = "changeType";
    roles[IsDirectoryRole] = "isDirectory";
    roles[IsCheckedRole] = "isChecked";
    roles[StatusFlagsRole] = "statusFlags";
    return roles;
}

/**
 * @brief モデルデータの構築
 *
 * ファイル変更リストからツリー構造のモデルデータを構築する
 * 重複を除外し、ディレクトリ階層を適切に生成
 *
 * @param changeOutput Snapperから返された改行区切りの変更出力
 * @param flatMode trueの場合はフラット表示用に構築する
 */
void FileChangeModel::setupModelData(const QString &changeOutput, bool flatMode)
{
    m_flatMode = flatMode;
    QElapsedTimer parsePreparationTimer;
    parsePreparationTimer.start();

    QVector<filechangemodel_detail::ChangeInfo> changes;
    QSet<QString> seenPaths;
    QSet<QString> parentPaths;
    const QStringList lines = changeOutput.split('\n', Qt::SkipEmptyParts);
    changes.reserve(lines.size());

    for (const QString &line : lines) {
        filechangemodel_detail::ChangeInfo info;
        if (!filechangemodel_detail::parseChangeRecord(line, &info) || seenPaths.contains(info.path)) {
            continue;
        }

        info.type = parseChangeType(info.statusFlags.at(0));
        seenPaths.insert(info.path);
        filechangemodel_detail::collectParentPaths(info.path, &parentPaths);
        changes.append(info);
    }

    QVector<filechangemodel_detail::ChangeInfo> treeChanges = changes;
    if (!flatMode) {
        std::sort(treeChanges.begin(), treeChanges.end(), [](const filechangemodel_detail::ChangeInfo &left, const filechangemodel_detail::ChangeInfo &right) {
            return left.path < right.path;
        });
    }
    const qint64 parsePreparationMs = parsePreparationTimer.elapsed();

    QElapsedTimer detachedTreeConstructionTimer;
    detachedTreeConstructionTimer.start();
    FileChangeItem *newRootItem = new FileChangeItem("", FileChangeItem::Modified);

    // --- フラットモード: 2 スナップショット比較ダイアログ用 ---
    // ListViewは、QAbstractItemModelのルート直下しか反復しないため、各変更をm_rootItemの直接の子として1行ずつ追加する
    // 中間ディレクトリは生成せず、重複除去のみ行う
    if (flatMode) {
        for (const filechangemodel_detail::ChangeInfo &info : changes) {
            const QString itemPath = info.isDirectory ? info.path + "/" : info.path;
            FileChangeItem *item = new FileChangeItem(itemPath, info.type, info.statusFlags, newRootItem);
            newRootItem->appendChild(item);
        }
    }
    else {
        // --- ツリーモード (従来): 対カレント比較 / 復元UI用 ---
        // アイテムマップ：正規化パス (スラッシュなし) --> FileChangeItem
        QHash<QString, FileChangeItem*> itemMap;
        itemMap[""] = newRootItem;

        // 変更リストをパス順に処理してツリーを構築する
        // 親パスは解析時に収集済みであり、全パスの接頭辞を総当たりする必要はない
        for (const filechangemodel_detail::ChangeInfo &info : treeChanges) {
            QStringList pathParts = info.path.split('/', Qt::SkipEmptyParts);
            QString currentPath = "";
            FileChangeItem *parentItem = newRootItem;

            for (int i = 0; i < pathParts.size(); ++i) {
                QString part = pathParts[i];
                currentPath += "/" + part;

                bool isLastPart = (i == pathParts.size() - 1);

                if (isLastPart) {
                    // 最終パート：変更があったファイルまたはディレクトリ
                    if (!itemMap.contains(currentPath)) {
                        const bool isDirectory = info.isDirectory || parentPaths.contains(info.path);
                        QString itemPath = isDirectory ? (currentPath + "/") : currentPath;
                        FileChangeItem *item = new FileChangeItem(itemPath, info.type, info.statusFlags, parentItem);
                        parentItem->appendChild(item);
                        itemMap[currentPath] = item;
                    }
                }
                else {
                    // 中間ディレクトリの処理
                    if (!itemMap.contains(currentPath)) {
                        FileChangeItem *dirItem = new FileChangeItem(currentPath + "/", FileChangeItem::Modified, QString(), parentItem);
                        parentItem->appendChild(dirItem);
                        itemMap[currentPath] = dirItem;
                    }
                    parentItem = itemMap[currentPath];
                }
            }
        }
    }

    const qint64 detachedTreeConstructionMs = detachedTreeConstructionTimer.elapsed();
    QElapsedTimer modelPublicationTimer;
    modelPublicationTimer.start();
    FileChangeItem *oldRootItem = m_rootItem;
    beginResetModel();
    m_rootItem = newRootItem;
    endResetModel();
    const qint64 modelPublicationMs = modelPublicationTimer.elapsed();
    delete oldRootItem;

    // この関数はロード成功後のモデル公開点であり、ここで初めて復元可能な由来を確定する
    m_loadedComparison = currentComparisonContext();
    m_loadedComparisonValid = true;

    qInfo() << "FileChangeModel timing: responseParsePreparation=" << parsePreparationMs
            << "ms detachedTreeConstruction=" << detachedTreeConstructionMs
            << "ms modelResetPublication=" << modelPublicationMs
            << "ms records=" << changes.size();
}

/**
 * @brief インデックスからアイテムを取得
 *
 * QModelIndexに対応するFileChangeItemを返す
 *
 * @param index QModelIndex
 * @return FileChangeItemへのポインタ (無効なインデックスの場合はルートアイテム)
 */
FileChangeItem *FileChangeModel::getItem(const QModelIndex &index) const
{
    if (index.isValid()) {
        FileChangeItem *item = static_cast<FileChangeItem*>(index.internalPointer());
        if (item) {
            return item;
        }
    }
    return m_rootItem;
}

/**
 * @brief 変更タイプをパース
 *
 * Snapperのステータス文字から変更タイプを判定する
 *
 * @param statusChar ステータス文字 ('+', '-', 'c', 'm', 't'など)
 * @return 変更タイプ
 */
FileChangeItem::ChangeType FileChangeModel::parseChangeType(const QChar &statusChar)
{
    switch (statusChar.toLatin1()) {
    case '+':
        return FileChangeItem::Created;
    case '-':
        return FileChangeItem::Deleted;
    case 'c':
    case 'm':
        return FileChangeItem::Modified;
    case 't':
        return FileChangeItem::TypeChanged;
    default:
        return FileChangeItem::Modified;
    }
}

/**
 * @brief アイテムのチェック状態を設定
 *
 * 指定されたパスのアイテムのチェック状態を設定する
 * ディレクトリの場合は配下の全てのアイテムも再帰的に設定される
 *
 * チェックを外す場合は、明示的にチェックを外したフラグが立てられる
 *
 * @param filePath ファイルパス
 * @param checked チェック状態 (true/false)
 */
void FileChangeModel::setItemChecked(const QString &filePath, bool checked)
{
    // ルートアイテムから指定されたパスのアイテムを検索
    QModelIndex index = findItemIndex(m_rootItem, filePath);
    if (index.isValid()) {
        FileChangeItem *item = getItem(index);

        // チェックを外す場合は、明示的にチェックを外したフラグを立てる
        if (!checked) {
            item->setExplicitlyUnchecked(true);
        }
        else {
            // チェックを入れる場合は、フラグをクリア
            item->setExplicitlyUnchecked(false);
        }

        setItemCheckedRecursive(item, index, checked);
    }
}

/**
 * @brief アイテムのチェック状態を再帰的に設定
 *
 * 指定されたアイテムとその配下の全てのアイテムのチェック状態を再帰的に設定する
 * 明示的にチェックを外された子アイテムはスキップされる
 *
 * @param item 対象のFileChangeItem
 * @param index 対象のQModelIndex
 * @param checked チェック状態 (true/false)
 */
void FileChangeModel::setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked)
{
    if (!item) {
        return;
    }

    // 現在のアイテムのチェック状態を設定
    item->setChecked(checked);
    emit dataChanged(index, index, {IsCheckedRole});

    // ディレクトリの場合、子要素を再帰的にチェック/アンチェック
    if (item->isDirectory()) {
        for (int i = 0; i < item->childCount(); ++i) {
            FileChangeItem *child = item->child(i);

            // チェックを入れる場合、明示的にチェックを外された子アイテムはスキップ
            if (checked && child->isExplicitlyUnchecked()) {
                continue;
            }

            QModelIndex childIndex = this->index(i, 0, index);
            setItemCheckedRecursive(child, childIndex, checked);
        }
    }
}

/**
 * @brief チェックされたアイテムのリストを取得
 *
 * チェックされた全てのアイテムのパスを収集し、復元順序に最適化してリストを返す
 * ディレクトリ階層の深い順にソートされる
 *
 * @return チェックされたアイテムのパスリスト
 */
QStringList FileChangeModel::getCheckedItems() const
{
    QStringList checkedPaths;
    collectCheckedItems(m_rootItem, checkedPaths);

    // 重複を除外
    QSet<QString> uniquePaths(checkedPaths.begin(), checkedPaths.end());
    checkedPaths = QStringList(uniquePaths.begin(), uniquePaths.end());

    // ディレクトリかファイルかを判定し、分類
    QStringList directories;
    QStringList files;

    for (const QString &path : checkedPaths) {
        // パスが他のパスの親である場合はディレクトリ
        bool isDirectory = false;
        for (const QString &otherPath : checkedPaths) {
            if (otherPath != path && otherPath.startsWith(path + "/")) {
                isDirectory = true;
                break;
            }
        }

        if (isDirectory) {
            directories.append(path);
        }
        else {
            files.append(path);
        }
    }

    // 復元順序を最適化：深い階層から浅い階層へソート
    // 深さでソート (スラッシュの数が多い方が深い)
    auto sortByDepth = [](const QString &a, const QString &b) {
        int depthA = a.count('/');
        int depthB = b.count('/');
        if (depthA != depthB) {
            return depthA > depthB; // 深い方が先
        }
        return a > b; // 同じ深さなら辞書順の逆順
    };

    std::sort(directories.begin(), directories.end(), sortByDepth);
    std::sort(files.begin(), files.end(), sortByDepth);

    // 復元リストを構築：ファイル --> ディレクトリの順
    // (深い階層から浅い階層へ)
    QStringList sortedPaths;
    sortedPaths.append(files);
    sortedPaths.append(directories);

    return sortedPaths;
}

/**
 * @brief 復元バッチサイズを更新する
 *
 * 入力値を 1..1000 に丸めた上で保持し、設定ファイルへ永続化する
 *
 * @param size ユーザが要求したバッチサイズ
 */
void FileChangeModel::setRestoreBatchSize(int size)
{
    // UIや設定値の揺れを吸収するため、有効範囲へ丸める
    size = qBound(1, size, 1000);
    if (m_restoreBatchSize != size) {
        m_restoreBatchSize = size;
        QSettings settings("Presire", "qSnapper");
        settings.setValue("restore/batchSize", m_restoreBatchSize);
        emit restoreBatchSizeChanged();
    }
}

/**
 * @brief 直接復元モードの有効 / 無効を更新する
 *
 * 値が変化した場合のみ内部状態と設定ファイルを更新する
 *
 * @param use trueなら直接復元を使用する
 */
void FileChangeModel::setUseDirectRestore(bool use)
{
    if (m_useDirectRestore != use) {
        m_useDirectRestore = use;
        QSettings settings("Presire", "qSnapper");
        settings.setValue("restore/useDirectMethod", m_useDirectRestore);
        emit useDirectRestoreChanged();
    }
}

/**
 * @brief アイテムのインデックスを検索
 *
 * 指定されたパスのアイテムを再帰的に検索し、そのQModelIndexを返す
 *
 * @param parent 検索開始アイテム
 * @param path 検索するパス
 * @return 見つかったアイテムのQModelIndex (見つからない場合は無効なインデックス)
 */
QModelIndex FileChangeModel::findItemIndex(FileChangeItem *parent, const QString &path) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);
        if (child->path() == path) {
            return createIndex(i, 0, child);
        }

        // 再帰的に子要素を検索
        QModelIndex childIndex = findItemIndex(child, path);
        if (childIndex.isValid()) {
            return childIndex;
        }
    }

    return QModelIndex();
}

/**
 * @brief チェックされたアイテムを収集
 *
 * チェックされたアイテムのパスを再帰的に収集する
 * ディレクトリの場合は、配下の全てのファイルも収集される
 *
 * @param parent 収集開始アイテム
 * @param paths 収集されたパスのリスト (出力)
 */
void FileChangeModel::collectCheckedItems(FileChangeItem *parent, QStringList &paths) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        if (child->isChecked()) {
            QString itemPath = child->path();

            // パスを正規化 (末尾のスラッシュを削除)
            if (itemPath.endsWith('/') && itemPath.length() > 1) {
                itemPath = itemPath.left(itemPath.length() - 1);
            }

            bool hasChildren = (child->childCount() > 0);
            bool isActualChange = (child->changeType() != FileChangeItem::Modified);

            if (hasChildren) {
                // 子要素があるアイテム = ディレクトリ

                // 実際に変更されたディレクトリのみ追加
                if (isActualChange && !itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                }

                // 配下を再帰的に収集
                collectAllFilesRecursive(child, paths);
            }
            else {
                // 子要素がないアイテム
                // パスと変更タイプから判断
                if (!itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                }
            }
        }
        else {
            // チェックされていないアイテムでも、子要素を再帰的に確認
            collectCheckedItems(child, paths);
        }
    }
}

/**
 * @brief 全てのファイルを再帰的に収集
 *
 * 指定されたアイテム配下の全てのファイルとディレクトリを再帰的に収集する
 * チェックが外されているアイテムはスキップされる
 *
 * @param parent 収集開始アイテム
 * @param paths 収集されたパスのリスト (出力)
 */
void FileChangeModel::collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        // チェックが外されているアイテムはスキップ
        if (!child->isChecked()) {
            continue;
        }

        QString itemPath = child->path();

        // パスを正規化 (末尾のスラッシュを削除)
        if (itemPath.endsWith('/') && itemPath.length() > 1) {
            itemPath = itemPath.left(itemPath.length() - 1);
        }

        bool hasChildren = (child->childCount() > 0);
        bool isActualChange = (child->changeType() != FileChangeItem::Modified);

        if (hasChildren) {
            // 子要素があるアイテム = ディレクトリ

            // 実際に変更されたディレクトリのみ追加
            if (isActualChange && !itemPath.isEmpty() && itemPath != "/") {
                paths.append(itemPath);
            }

            // さらに配下を再帰的に処理
            collectAllFilesRecursive(child, paths);
        }
        else {
            // 子要素がないアイテム
            if (!itemPath.isEmpty() && itemPath != "/") {
                paths.append(itemPath);
            }
        }
    }
}
