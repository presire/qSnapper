#ifndef SNAPSHOTLISTMODEL_H
#define SNAPSHOTLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "fssnapshot.h"

class SnapperService;

class SnapshotListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum SnapshotRoles {
        NumberRole = Qt::UserRole + 1,
        SnapshotTypeRole,
        PreviousNumberRole,
        TimestampRole,
        UserRole,
        CleanupAlgoRole,
        DescriptionRole,
        SnapshotTypeStringRole,
        CleanupAlgoStringRole,
        UserdataRole
    };

    explicit SnapshotListModel(QObject *parent = nullptr);
    ~SnapshotListModel();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_snapshots.count(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void createSingleSnapshot(const QString &description);
    Q_INVOKABLE void createPreSnapshot(const QString &description);
    Q_INVOKABLE void createPostSnapshot(const QString &description, int previousNumber);
    Q_INVOKABLE void rollbackSnapshot(int number);
    Q_INVOKABLE void deleteSnapshot(int number);
    Q_INVOKABLE void deleteSnapshots(const QVariantList &numbers);

signals:
    void countChanged();
    void snapshotCreated();
    void snapshotCreationFailed(const QString &error);
    void rollbackCompleted();
    void rollbackFailed(const QString &error);
    void snapshotDeleted(int number);
    void snapshotDeletionFailed(int number, const QString &error);
    void snapshotsDeletionCompleted(int successCount, int failureCount);

private slots:
    void onSnapshotCreated(FsSnapshot *snapshot);
    void onSnapshotCreationFailed(const QString &error);
    void onRollbackCompleted();
    void onRollbackFailed(const QString &error);
    void onSnapshotDeleted(int number);
    void onSnapshotDeletionFailed(int number, const QString &error);

private:
    QList<FsSnapshot*> m_snapshots;      // スナップショットオブジェクトのリスト
    SnapperService *m_snapperService;    // SnapperServiceシングルトンインスタンスへのポインタ
};

#endif // SNAPSHOTLISTMODEL_H
