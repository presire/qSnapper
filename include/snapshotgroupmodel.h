#ifndef SNAPSHOTGROUPMODEL_H
#define SNAPSHOTGROUPMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QVariantMap>
#include <QList>

#include "snapshotlistmodel.h"

// Pre/Postペアをグループ化して表示するためのモデル
// SnapshotListModelをソースとし、Pre/Postペアを1行にまとめる
class SnapshotGroupModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(SnapshotListModel* snapshotListModel READ snapshotListModel WRITE setSnapshotListModel NOTIFY snapshotListModelChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum GroupType {
        Single = 0,
        PrePost = 1,
        UnpairedPre = 2,
        OrphanPost = 3
    };
    Q_ENUM(GroupType)

    enum GroupRoles {
        DisplayIdRole = Qt::UserRole + 1,
        GroupTypeRole,
        GroupTypeStringRole,
        StartTimeRole,
        EndTimeRole,
        DescriptionRole,
        UserRole,
        UserdataRole,
        PreNumberRole,
        PostNumberRole,
        CleanupAlgoStringRole,
        IsImportantRole
    };

    explicit SnapshotGroupModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    SnapshotListModel *snapshotListModel() const;
    void setSnapshotListModel(SnapshotListModel *model);

    Q_INVOKABLE QVariantList snapshotNumbersAt(int row) const;

signals:
    void snapshotListModelChanged();
    void countChanged();

private slots:
    void rebuild();

private:
    struct SnapshotGroup {
        GroupType groupType;
        int preNumber;
        int postNumber;
        QDateTime startTime;
        QDateTime endTime;
        QString description;
        QString user;
        QVariantMap userdata;
        QString cleanupAlgoString;
        bool isImportant;
    };

    SnapshotListModel *m_snapshotListModel = nullptr;
    QList<SnapshotGroup> m_groups;
};

#endif // SNAPSHOTGROUPMODEL_H
