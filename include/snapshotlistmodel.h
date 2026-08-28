#ifndef SNAPSHOTLISTMODEL_H
#define SNAPSHOTLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include "fssnapshot.h"

class SnapperService;

/**
 * @brief スナップショット一覧を提供するリストモデル
 *
 * SnapperServiceから取得したFsSnapshotリストをQAbstractListModelとしてQMLへ公開する
 * 作成・削除・ロールバック・変更操作をQMLから呼び出し可能にし、件数プロパティやロール定義を通じてViewとのバインディングを担う
 */
class SnapshotListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)    // スナップショット件数

public:
    // スナップショットロールを表す列挙型
    enum SnapshotRoles {
        NumberRole = Qt::UserRole + 1,  // 番号ロール
        SnapshotTypeRole,               // スナップショット種別ロール
        PreviousNumberRole,             // 紐付け元番号ロール
        TimestampRole,                  // タイムスタンプロール
        UserRole,                       // ユーザ名ロール
        CleanupAlgoRole,                // クリーンアップアルゴリズムロール
        DescriptionRole,                // 説明文ロール
        SnapshotTypeStringRole,         // スナップショット種別文字列ロール
        CleanupAlgoStringRole,          // クリーンアップアルゴリズム文字列ロール
        UserdataRole                    // ユーザデータロール
    };

    // コンストラクタ / デストラクタ
    explicit SnapshotListModel(QObject *parent = nullptr);  // コンストラクタ
    ~SnapshotListModel();                                   // デストラクタ

    // モデルインターフェース
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;             // 行数を取得する
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override; // 指定インデックスのデータを取得する
    QHash<int, QByteArray> roleNames() const override;                                  // ロール名マップを取得する

    // プロパティゲッター
    int count() const { return m_snapshots.count(); }                                   // スナップショット件数を取得する

    // QML公開操作 (更新・作成・操作)
    Q_INVOKABLE void refresh();                                                         // 一覧を更新する
    Q_INVOKABLE void createSingleSnapshot(const QString &description,                   // 単発スナップショットを作成する
                                          const QVariantMap &userdata = QVariantMap());
    Q_INVOKABLE void createPreSnapshot(const QString &description,                      // Preスナップショットを作成する
                                       const QVariantMap &userdata = QVariantMap());
    Q_INVOKABLE void createPostSnapshot(const QString &description,                     // Postスナップショットを作成する
                                        int previousNumber,
                                        const QVariantMap &userdata = QVariantMap());
    Q_INVOKABLE void rollbackSnapshot(int number);                                      // 指定番号へロールバックする
    Q_INVOKABLE void deleteSnapshot(int number);                                        // 単一スナップショットを削除する
    Q_INVOKABLE void deleteSnapshots(const QVariantList &numbers);                      // 複数スナップショットを削除する
    Q_INVOKABLE void modifySnapshot(int number,                                         // スナップショット情報を変更する
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata);

signals:
    void countChanged();                                                    // 件数が変更されたときに発行する
    void snapshotCreated();                                                 // スナップショット作成成功時に発行する
    void snapshotCreationFailed(const QString &error);                      // スナップショット作成失敗時に発行する
    void rollbackCompleted();                                               // ロールバック完了時に発行する
    void rollbackFailed(const QString &error);                              // ロールバック失敗時に発行する
    void snapshotDeleted(int number);                                       // 単一削除成功時に発行する
    void snapshotDeletionFailed(int number, const QString &error);          // 単一削除失敗時に発行する
    void snapshotsDeletionCompleted(int successCount, int failureCount);    // 複数削除完了時に発行する
    void snapshotModified(int number);                                      // 変更成功時に発行する
    void snapshotModificationFailed(int number, const QString &error);      // 変更失敗時に発行する

private slots:
    void onSnapshotCreated(FsSnapshot *snapshot);                           // 作成成功通知を処理する
    void onSnapshotCreationFailed(const QString &error);                    // 作成失敗通知を処理する
    void onRollbackCompleted();                                             // ロールバック完了通知を処理する
    void onRollbackFailed(const QString &error);                            // ロールバック失敗通知を処理する
    void onSnapshotDeleted(int number);                                     // 削除成功通知を処理する
    void onSnapshotDeletionFailed(int number, const QString &error);        // 削除失敗通知を処理する

private:
    // モデルデータ
    QList<FsSnapshot*> m_snapshots;     // スナップショットオブジェクトのリスト
    SnapperService *m_snapperService;   // SnapperServiceシングルトンインスタンスへのポインタ
};

#endif // SNAPSHOTLISTMODEL_H
