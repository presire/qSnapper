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
    QString m_statusFlags;                  // 詳細ステータスフラグ (例: "cpu..")
    QVector<FileChangeItem*> m_children;    // 子要素のリスト
    FileChangeItem *m_parent;               // 親要素へのポインタ
    bool m_checked = false;                 // チェック状態
    bool m_explicitlyUnchecked = false;     // 明示的にチェックを外されたフラグ

public:
    explicit FileChangeItem(const QString &path, ChangeType type, const QString &statusFlags = QString(), FileChangeItem *parent = nullptr);
    ~FileChangeItem();

    void appendChild(FileChangeItem *child);
    FileChangeItem *child(int row);
    int childCount() const;
    int row() const;
    FileChangeItem *parent();

    QString path() const { return m_path; }
    QString name() const;
    ChangeType changeType() const { return m_changeType; }
    QString statusFlags() const { return m_statusFlags; }
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
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int restoreBatchSize READ restoreBatchSize WRITE setRestoreBatchSize NOTIFY restoreBatchSizeChanged)
    Q_PROPERTY(bool useDirectRestore READ useDirectRestore WRITE setUseDirectRestore NOTIFY useDirectRestoreChanged)

private:
    QString m_configName;                   // Snapper設定名
    int m_snapshotNumber;                   // スナップショット番号 (対カレント比較モード用)
    int m_compareNumber1;                   // 比較元スナップショット番号 (任意2スナップショット比較モード用)
    int m_compareNumber2;                   // 比較先スナップショット番号 (同上)
    bool m_betweenMode;                     // 任意2スナップショット比較モードかどうか
    bool m_flatMode;                        // フラット表示モード (ツリー構築をバイパスし、
                                            // 各変更を m_rootItem 直下に1行ずつ追加する)
                                            // 比較ダイアログ用 (ListView 表示)
    FileChangeItem *m_rootItem;             // ツリーのルートアイテム
    QDBusInterface *m_dbusInterface;        // D-Busインターフェース
    bool m_hasChanges;                      // ファイル変更があるかどうか
    bool m_loading;                         // 読み込み中フラグ

    // バッチ復元用の変数
    QList<QStringList> m_restoreBatches;    // 復元ファイルのバッチリスト
    QList<QStringList> m_restoreBatchChangeTypes; // バッチごとの変更タイプリスト
    int m_currentBatchIndex;                // 現在処理中のバッチインデックス
    int m_totalFilesCount;                  // 復元対象の総ファイル数
    int m_processedFilesCount;              // 処理済みファイル数
    bool m_restoreHasError;                 // 復元エラーフラグ
    bool m_cancelRequested;                 // キャンセル要求フラグ

    // 復元設定
    int m_restoreBatchSize;                 // バッチサイズ (1-1000)
    bool m_useDirectRestore;                // Direct Copy方式を使用するか

public:

private:
    void clearModel();
    FileChangeItem *getItem(const QModelIndex &index) const;
    FileChangeItem::ChangeType parseChangeType(const QChar &statusChar);
    QModelIndex findItemIndex(FileChangeItem *parent, const QString &path) const;
    void collectCheckedItems(FileChangeItem *parent, QStringList &paths) const;
    void collectCheckedItemsWithTypes(FileChangeItem *parent, QStringList &paths, QStringList &changeTypes) const;
    static QString changeTypeToString(FileChangeItem::ChangeType type);
    void collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const;
    void setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked);
    void processNextBatch();
    bool reconnectDbus();                    // D-Busサービスへの再接続を試みる

protected:
    /**
     * @brief 変更レコードからモデルデータを構築する
     *
     * レガシーの改行区切りステータス出力を1回だけ解析し、指定した表示モードの
     * モデルツリーを構築する。テストからもD-Busを介さずに検証できるよう保護する。
     *
     * @param changeOutput Snapperの変更出力
     * @param flatMode trueの場合はフラットモデルを構築する
     */
    void setupModelData(const QString &changeOutput, bool flatMode);

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        ChangeTypeRole,
        IsDirectoryRole,
        IsCheckedRole,
        StatusFlagsRole
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
    bool isLoading() const { return m_loading; }

    int restoreBatchSize() const { return m_restoreBatchSize; }
    void setRestoreBatchSize(int size);

    bool useDirectRestore() const { return m_useDirectRestore; }
    void setUseDirectRestore(bool use);

    // 公開メソッド
    Q_INVOKABLE void loadChanges();
    Q_INVOKABLE void loadChangesBetween(int number1, int number2, bool flat = false);
    Q_INVOKABLE void getFileDiffAndDetails(const QString &filePath);
    Q_INVOKABLE void setItemChecked(const QString &filePath, bool checked);
    Q_INVOKABLE QStringList getCheckedItems() const;
    Q_INVOKABLE bool restoreCheckedItems();
    Q_INVOKABLE void restoreSingleFile(const QString &filePath);
    Q_INVOKABLE void cancelRestore();

signals:
    void configNameChanged();
    void snapshotNumberChanged();
    void hasChangesChanged();
    void loadingChanged();
    void errorOccurred(const QString &message);
    void fileDiffAndDetailsReady(const QString &filePath, const QVariantMap &details, const QString &diff);
    void restoreProgress(int current, int total, const QString &filePath);
    void restoreCompleted(bool success);
    void restoreBatchSizeChanged();
    void useDirectRestoreChanged();

private slots:
    void onRestoreProgress(int current, int total, const QString &filePath);
};

#endif // FILECHANGEMODEL_H
