#ifndef SNAPSHOTGROUPMODEL_H
#define SNAPSHOTGROUPMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QVariantMap>
#include <QList>

#include "snapshotlistmodel.h"

/**
 * @brief Pre/Postペアをグループ化して表示するためのモデル
 *
 * SnapshotListModelをソースとし、Pre/Postペアを1行にまとめて表示するプロキシモデル
 * 単発、Pre/Postペア、未ペアPre、孤立Postの4種別に分類し、開始 / 終了時刻や説明などをグループ単位で提供する
 */
class SnapshotGroupModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(SnapshotListModel* snapshotListModel READ snapshotListModel              // ソースとなるSnapshotListModel
                                                    WRITE setSnapshotListModel
                                                    NOTIFY snapshotListModelChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)                                // グループ件数

public:
    // グループ種別を表す列挙型
    enum GroupType {
        Single      = 0,    // 単発グループ
        PrePost     = 1,    // Pre/Postペアグループ
        UnpairedPre = 2,    // 未ペアPreグループ
        OrphanPost  = 3     // 孤立Postグループ
    };
    Q_ENUM(GroupType)

    // グループロールを表す列挙型
    enum GroupRoles {
        DisplayIdRole = Qt::UserRole + 1,   // 表示IDロール
        GroupTypeRole,                      // グループ種別ロール
        GroupTypeStringRole,                // グループ種別文字列ロール
        StartTimeRole,                      // 開始時刻ロール
        EndTimeRole,                        // 終了時刻ロール
        DescriptionRole,                    // 説明文ロール
        UserRole,                           // ユーザ名ロール
        UserdataRole,                       // ユーザデータロール
        PreNumberRole,                      // Pre番号ロール
        PostNumberRole,                     // Post番号ロール
        CleanupAlgoStringRole,              // クリーンアップアルゴリズム文字列ロール
        IsImportantRole                     // 重要フラグロール
    };

    // コンストラクタ
    explicit SnapshotGroupModel(QObject *parent = nullptr);                 // コンストラクタ

    // モデルインターフェース
    int rowCount(const QModelIndex &parent = QModelIndex()) const override; // 行数を取得する
    QVariant data(const QModelIndex &index,                                 // 指定インデックスのデータを取得する
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;                      // ロール名マップを取得する

    // プロパティゲッター / セッター
    int count() const;                                                      // グループ件数を取得する
    SnapshotListModel *snapshotListModel() const;                           // ソースモデルを取得する
    void setSnapshotListModel(SnapshotListModel *model);                    // ソースモデルを設定する

    // QML公開操作
    Q_INVOKABLE QVariantList snapshotNumbersAt(int row) const;              // 指定行のスナップショット番号一覧を取得する

signals:
    void snapshotListModelChanged();    // ソースモデル変更時に発行する
    void countChanged();                // 件数変更時に発行する

private slots:
    void rebuild();                     // グループを再構築する

private:
    // グループ内部構造体
    struct SnapshotGroup {
        GroupType groupType;        // グループ種別
        int preNumber;              // Preスナップショット番号
        int postNumber;             // Postスナップショット番号
        QDateTime startTime;        // 開始時刻
        QDateTime endTime;          // 終了時刻
        QString description;        // 説明文
        QString user;               // ユーザ名
        QVariantMap userdata;       // ユーザデータ
        QString cleanupAlgoString;  // クリーンアップアルゴリズム文字列
        bool isImportant;           // 重要フラグ
    };

    // モデルデータ
    SnapshotListModel *m_snapshotListModel = nullptr;   // ソースとなるSnapshotListModelへのポインタ
    QList<SnapshotGroup> m_groups;                      // グループ化されたスナップショットリスト
};

#endif // SNAPSHOTGROUPMODEL_H
