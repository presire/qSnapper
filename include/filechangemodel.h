#ifndef FILECHANGEMODEL_H
#define FILECHANGEMODEL_H

#include <QAbstractItemModel>
#include <QVariantMap>
#include <QString>
#include <QVector>
#include <QDBusInterface>

/**
 * @brief ファイル変更情報を保持するアイテムクラス
 */
class FileChangeItem
{
public:
    enum ChangeType {
        Created,    // 新規作成
        Modified,   // 変更
        Deleted,    // 削除
        TypeChanged // タイプ変更
    };

private:
    QString m_path;                         // ファイル/ディレクトリのパス
    ChangeType m_changeType;                // 変更タイプ
    QVector<FileChangeItem*> m_children;    // 子要素のリスト
    FileChangeItem *m_parent;               // 親要素へのポインタ
    bool m_checked = false;                 // チェック状態
    bool m_explicitlyUnchecked = false;     // 明示的にチェックを外されたフラグ

public:
    explicit FileChangeItem(const QString &path, ChangeType type, FileChangeItem *parent = nullptr);
    ~FileChangeItem();

    void appendChild(FileChangeItem *child);
    FileChangeItem *child(int row);
    int childCount() const;
    int row() const;
    FileChangeItem *parent();

    QString path() const { return m_path; }
    QString name() const;
    ChangeType changeType() const { return m_changeType; }
    bool isDirectory() const;
    bool isChecked() const { return m_checked; }
    void setChecked(bool checked) { m_checked = checked; }
    bool isExplicitlyUnchecked() const { return m_explicitlyUnchecked; }
    void setExplicitlyUnchecked(bool explicitlyUnchecked) { m_explicitlyUnchecked = explicitlyUnchecked; }
};

/**
 * @brief ファイル変更をツリー構造で表示するためのモデル
 */
class FileChangeModel : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QString configName READ configName WRITE setConfigName NOTIFY configNameChanged)
    Q_PROPERTY(int snapshotNumber READ snapshotNumber WRITE setSnapshotNumber NOTIFY snapshotNumberChanged)
    Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY hasChangesChanged)

private:
    QString m_configName;                   // Snapper設定名
    int m_snapshotNumber;                   // スナップショット番号
    FileChangeItem *m_rootItem;             // ツリーのルートアイテム
    QDBusInterface *m_dbusInterface;        // D-Busインターフェース
    bool m_hasChanges;                      // ファイル変更があるかどうか

    // バッチ復元用の変数
    QList<QStringList> m_restoreBatches;    // 復元ファイルのバッチリスト
    int m_currentBatchIndex;                // 現在処理中のバッチインデックス
    int m_totalFilesCount;                  // 復元対象の総ファイル数
    int m_processedFilesCount;              // 処理済みファイル数
    bool m_restoreHasError;                 // 復元エラーフラグ
    bool m_cancelRequested;                 // キャンセル要求フラグ

public:

private:
    void setupModelData(const QStringList &changes);
    void clearModel();
    FileChangeItem *getItem(const QModelIndex &index) const;
    FileChangeItem::ChangeType parseChangeType(const QChar &statusChar);
    QString executeCommand(const QString &command, const QStringList &arguments);
    QDBusInterface* getDBusInterface();
    QModelIndex findItemIndex(FileChangeItem *parent, const QString &path) const;
    void collectCheckedItems(FileChangeItem *parent, QStringList &paths) const;
    void collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const;
    void setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked);
    void processNextBatch();
    void dumpTree(FileChangeItem *item, int depth, int maxDepth);

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        ChangeTypeRole,
        IsDirectoryRole,
        IsCheckedRole
    };

    explicit FileChangeModel(QObject *parent = nullptr);
    ~FileChangeModel();

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // プロパティ
    QString configName() const { return m_configName; }
    void setConfigName(const QString &name);

    int snapshotNumber() const { return m_snapshotNumber; }
    void setSnapshotNumber(int number);

    bool hasChanges() const { return m_hasChanges; }

    // 公開メソッド
    Q_INVOKABLE void loadChanges();
    Q_INVOKABLE QString getFileDiff(const QString &filePath);
    Q_INVOKABLE void setItemChecked(const QString &filePath, bool checked);
    Q_INVOKABLE QStringList getCheckedItems() const;
    Q_INVOKABLE bool restoreCheckedItems();
    Q_INVOKABLE void cancelRestore();

signals:
    void configNameChanged();
    void snapshotNumberChanged();
    void hasChangesChanged();
    void errorOccurred(const QString &message);
    void restoreProgress(int current, int total, const QString &filePath);
    void restoreCompleted(bool success);

private slots:
    void onRestoreProgress(int current, int total, const QString &filePath);
};

#endif // FILECHANGEMODEL_H
