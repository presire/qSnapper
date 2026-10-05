#ifndef SNAPSHOTGROUPMODEL_H
#define SNAPSHOTGROUPMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QVariantMap>
#include <QList>

#include "snapshotlistmodel.h"

/**
 * @file snapshotgroupmodel.h
 * @brief Pre/Postペアを1行にまとめるプロキシモデル
 */

/**
 * @class SnapshotGroupModel
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
    /**
     * @enum GroupType
     * @brief グループの種別
     */
    enum GroupType {
        Single      = 0,    // 単発グループ
        PrePost     = 1,    // Pre/Postペアグループ
        UnpairedPre = 2,    // 未ペアPreグループ
        OrphanPost  = 3     // 孤立Postグループ
    };
    Q_ENUM(GroupType)

    /**
     * @enum GroupRoles
     * @brief QMLへ公開するグループのロール
     */
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

    /**
     * @brief SnapshotGroupModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit SnapshotGroupModel(QObject *parent = nullptr);

    /**
     * @brief 行数を取得する
     * @param parent 親インデックス (リストモデルでは未使用)
     * @return グループ件数
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    /**
     * @brief 指定インデックスのデータを取得する
     * @param index 対象インデックス
     * @param role 取得するロール
     * @return ロールに対応する値、範囲外の場合は無効なQVariant
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    /**
     * @brief ロール名マップを取得する
     * @return ロール番号からロール名へのマップ
     */
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief グループ件数を取得する
     * @return グループ件数
     */
    int count() const;

    /**
     * @brief ソースモデルを取得する
     * @return ソースとなるSnapshotListModel
     */
    SnapshotListModel *snapshotListModel() const;

    /**
     * @brief ソースモデルを設定する
     *
     * 設定後はソースの変更を監視し、変化のたびにグループを再構築する
     *
     * @param model 新たなソースモデル
     */
    void setSnapshotListModel(SnapshotListModel *model);

    /**
     * @brief 指定行のスナップショット番号一覧を取得する
     * @param row 対象行
     * @return 行が表すスナップショット番号の一覧、範囲外の場合は空のリスト
     */
    Q_INVOKABLE QVariantList snapshotNumbersAt(int row) const;

signals:
    /**
     * @brief ソースモデル変更時に発行する
     */
    void snapshotListModelChanged();

    /**
     * @brief 件数変更時に発行する
     */
    void countChanged();

private slots:
    /**
     * @brief グループを再構築する
     *
     * ソースモデルの全スナップショットを走査し、Pre/Postの対応付けをやり直す
     */
    void rebuild();

private:
    // グループ内部構造体
    /**
     * @struct SnapshotGroup
     * @brief 1行分に集約したグループの内容
     */
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
