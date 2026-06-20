#include "snapshotgroupmodel.h"
#include "snapshotlistmodel.h"
#include <QHash>

/**
 * @brief SnapshotGroupModelを初期化する
 *
 * 初期状態では snapshotListModel を持たず、空のグループ一覧を保持する
 *
 * @param parent 親QObject
 */
SnapshotGroupModel::SnapshotGroupModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

/**
 * @brief グループ数を返す
 *
 * リストモデルのため、子インデックスに対しては常に0を返す
 *
 * @param parent 親インデックス
 * @return ルート直下のグループ数、子インデックスに対しては0
 */
int SnapshotGroupModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return m_groups.count();
}

/**
 * @brief 指定行・ロールに対応するグループ情報を返す
 *
 * Pre/Postの組や単体スナップショットを、QMLで扱いやすいQVariantに変換する
 *
 * @param index 対象行のモデルインデックス
 * @param role 取得したいロール
 * @return ロールに対応する値、無効インデックスや未対応ロールでは空QVariant
 */
QVariant SnapshotGroupModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_groups.count())
        return QVariant();

    const SnapshotGroup &group = m_groups.at(index.row());

    switch (role) {
    case DisplayIdRole:
        if (group.groupType == PrePost)
            return QString("%1 & %2").arg(group.preNumber).arg(group.postNumber);
        return QString::number(group.preNumber);
    case GroupTypeRole:
        return static_cast<int>(group.groupType);
    case GroupTypeStringRole:
        switch (group.groupType) {
        case Single:      return QStringLiteral("single");
        case PrePost:     return QStringLiteral("prepost");
        case UnpairedPre: return QStringLiteral("pre");
        case OrphanPost:  return QStringLiteral("post");
        }
        return QStringLiteral("unknown");
    case StartTimeRole:
        return group.startTime;
    case EndTimeRole:
        return group.endTime;
    case DescriptionRole:
        return group.description;
    case UserRole:
        return group.user;
    case UserdataRole:
        return group.userdata;
    case PreNumberRole:
        return group.preNumber;
    case PostNumberRole:
        return group.postNumber;
    case CleanupAlgoStringRole:
        return group.cleanupAlgoString;
    case IsImportantRole:
        return group.isImportant;
    default:
        return QVariant();
    }
}

/**
 * @brief QML公開用のロール名一覧を返す
 *
 * @return ロールIDとロール名の対応表
 */
QHash<int, QByteArray> SnapshotGroupModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[DisplayIdRole] = "displayId";
    roles[GroupTypeRole] = "groupType";
    roles[GroupTypeStringRole] = "groupTypeString";
    roles[StartTimeRole] = "startTime";
    roles[EndTimeRole] = "endTime";
    roles[DescriptionRole] = "description";
    roles[UserRole] = "user";
    roles[UserdataRole] = "userdata";
    roles[PreNumberRole] = "preNumber";
    roles[PostNumberRole] = "postNumber";
    roles[CleanupAlgoStringRole] = "cleanupAlgoString";
    roles[IsImportantRole] = "isImportant";
    return roles;
}

/**
 * @brief 現在のグループ数を返す
 *
 * @return 保持しているSnapshotGroupの件数
 */
int SnapshotGroupModel::count() const
{
    return m_groups.count();
}

/**
 * @brief 変換元となるSnapshotListModelを返す
 *
 * @return 現在接続されているsnapshot list model、未設定時はnullptr
 */
SnapshotListModel *SnapshotGroupModel::snapshotListModel() const
{
    return m_snapshotListModel;
}

/**
 * @brief 変換元モデルを差し替える
 *
 * 既存接続を解除し、新しいsnapshot list modelの更新シグナルに接続して rebuild() を行う
 *
 * @param model 新しく接続するSnapshotListModel
 */
void SnapshotGroupModel::setSnapshotListModel(SnapshotListModel *model)
{
    if (m_snapshotListModel == model) {
        return;
    }

    if (m_snapshotListModel) {
        disconnect(m_snapshotListModel, nullptr, this, nullptr);
    }

    m_snapshotListModel = model;

    if (m_snapshotListModel) {
        connect(m_snapshotListModel, &QAbstractListModel::modelReset, this, &SnapshotGroupModel::rebuild);
        connect(m_snapshotListModel, &QAbstractListModel::rowsInserted, this, &SnapshotGroupModel::rebuild);
        connect(m_snapshotListModel, &QAbstractListModel::rowsRemoved, this, &SnapshotGroupModel::rebuild);
        rebuild();
    }

    emit snapshotListModelChanged();
}

/**
 * @brief 指定行に対応するスナップショット番号群を返す
 *
 * Pre/Postペアの場合は2要素、単体グループの場合は1要素を返す
 *
 * @param row 対象行
 * @return 対応するスナップショット番号の配列、範囲外では空配列
 */
QVariantList SnapshotGroupModel::snapshotNumbersAt(int row) const
{
    if (row < 0 || row >= m_groups.count()) {
        return {};
    }

    const SnapshotGroup &group = m_groups.at(row);
    QVariantList numbers;
    numbers.append(group.preNumber);
    if (group.postNumber > 0) {
        numbers.append(group.postNumber);
    }

    return numbers;
}

/**
 * @brief snapshot list modelからグループ一覧を再構築する
 *
 * 単体スナップショット、Pre/Postペア、未対応Pre、孤児Postを分類し、QML表示向けのSnapshotGroup配列へ変換する
 */
void SnapshotGroupModel::rebuild()
{
    if (!m_snapshotListModel)
        return;

    beginResetModel();
    m_groups.clear();

    const int rowCount = m_snapshotListModel->rowCount();
    const auto roles = m_snapshotListModel->roleNames();

    // ロールIDを取得
    int numberRole = -1, typeStringRole = -1, prevNumRole = -1;
    int timestampRole = -1, userRole = -1, descRole = -1;
    int userdataRole = -1, cleanupRole = -1;

    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        if (it.value() == "number") numberRole = it.key();
        else if (it.value() == "snapshotTypeString") typeStringRole = it.key();
        else if (it.value() == "previousNumber") prevNumRole = it.key();
        else if (it.value() == "timestamp") timestampRole = it.key();
        else if (it.value() == "user") userRole = it.key();
        else if (it.value() == "description") descRole = it.key();
        else if (it.value() == "userdata") userdataRole = it.key();
        else if (it.value() == "cleanupAlgoString") cleanupRole = it.key();
    }

    // 全スナップショットのデータを収集
    struct RawSnapshot {
        int number;
        QString typeString;
        int previousNumber;
        QDateTime timestamp;
        QString user;
        QString description;
        QVariantMap userdata;
        QString cleanupAlgoString;
    };

    QList<RawSnapshot> snapshots;
    QHash<int, int> snapshotIndexByNumber;

    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = m_snapshotListModel->index(i, 0);
        RawSnapshot s;
        s.number = m_snapshotListModel->data(idx, numberRole).toInt();
        s.typeString = m_snapshotListModel->data(idx, typeStringRole).toString();
        s.previousNumber = m_snapshotListModel->data(idx, prevNumRole).toInt();
        s.timestamp = m_snapshotListModel->data(idx, timestampRole).toDateTime();
        s.user = m_snapshotListModel->data(idx, userRole).toString();
        s.description = m_snapshotListModel->data(idx, descRole).toString();
        s.userdata = m_snapshotListModel->data(idx, userdataRole).toMap();
        s.cleanupAlgoString = m_snapshotListModel->data(idx, cleanupRole).toString();
        snapshots.append(s);
        snapshotIndexByNumber[s.number] = i;
    }

    // Post --> Preのマッピングを構築
    // key: Pre番号, value: Postのインデックス (snapshots配列内)
    QHash<int, int> postIndexByPreNumber;
    for (int i = 0; i < snapshots.count(); ++i) {
        const RawSnapshot &s = snapshots[i];
        if (s.typeString == "post" && s.previousNumber > 0) {
            postIndexByPreNumber[s.previousNumber] = i;
        }
    }

    // 消費済みPostを追跡
    QSet<int> consumedPostIndices;

    for (int i = 0; i < snapshots.count(); ++i) {
        const RawSnapshot &s = snapshots[i];

        if (s.typeString == "single") {
            SnapshotGroup group;
            group.groupType = Single;
            group.preNumber = s.number;
            group.postNumber = 0;
            group.startTime = s.timestamp;
            group.description = s.description;
            group.user = s.user;
            group.userdata = s.userdata;
            group.cleanupAlgoString = s.cleanupAlgoString;
            group.isImportant = s.userdata.value("important").toString() == "yes";
            m_groups.append(group);
        }
        else if (s.typeString == "pre") {
            SnapshotGroup group;
            group.preNumber = s.number;
            group.startTime = s.timestamp;
            group.user = s.user;
            group.cleanupAlgoString = s.cleanupAlgoString;

            if (postIndexByPreNumber.contains(s.number)) {
                int postIdx = postIndexByPreNumber[s.number];
                const RawSnapshot &post = snapshots[postIdx];
                group.groupType = PrePost;
                group.postNumber = post.number;
                group.endTime = post.timestamp;
                group.description = s.description.isEmpty() ? post.description : s.description;

                // userdataを統合 (Pre優先、Postで補完)
                group.userdata = s.userdata;

                for (auto it = post.userdata.constBegin(); it != post.userdata.constEnd(); ++it) {
                    if (!group.userdata.contains(it.key()))
                        group.userdata.insert(it.key(), it.value());
                }
                group.isImportant = s.userdata.value("important").toString() == "yes"
                                 || post.userdata.value("important").toString() == "yes";
                consumedPostIndices.insert(postIdx);
            }
            else {
                group.groupType = UnpairedPre;
                group.postNumber = 0;
                group.description = s.description;
                group.userdata = s.userdata;
                group.isImportant = s.userdata.value("important").toString() == "yes";
            }
            m_groups.append(group);
        }
        else if (s.typeString == "post") {
            if (!consumedPostIndices.contains(i)) {
                // 孤児Post (対応するPreがない)
                SnapshotGroup group;
                group.groupType = OrphanPost;
                group.preNumber = s.number;
                group.postNumber = 0;
                group.startTime = s.timestamp;
                group.description = s.description;
                group.user = s.user;
                group.userdata = s.userdata;
                group.cleanupAlgoString = s.cleanupAlgoString;
                group.isImportant = s.userdata.value("important").toString() == "yes";
                m_groups.append(group);
            }
        }
    }

    endResetModel();
    emit countChanged();
}
