#ifndef SNAPSHOTLISTMODEL_H
#define SNAPSHOTLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include "fssnapshot.h"

class SnapperService;

/**
 * @file snapshotlistmodel.h
 * @brief スナップショット一覧をQMLへ公開するリストモデル
 */

/**
 * @class SnapshotListModel
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
    /**
     * @enum SnapshotRoles
     * @brief QMLへ公開するスナップショットのロール
     */
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

    /**
     * @brief SnapshotListModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit SnapshotListModel(QObject *parent = nullptr);

    /**
     * @brief SnapshotListModelを破棄する
     */
    ~SnapshotListModel();

    /**
     * @brief 行数を取得する
     * @param parent 親インデックス (リストモデルでは未使用)
     * @return スナップショット件数
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
     * @brief スナップショット件数を取得する
     * @return スナップショット件数
     */
    int count() const { return m_snapshots.count(); }

    /**
     * @brief 一覧を更新する
     *
     * SnapperServiceから全スナップショットを取得し直してモデルへ反映する
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief 単発スナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createSingleSnapshot(const QString &description,
                                          const QString &cleanup,
                                          bool important,
                                          const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Preスナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createPreSnapshot(const QString &description,
                                       const QString &cleanup,
                                       bool important,
                                       const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Postスナップショットを作成する
     * @param description 説明文
     * @param previousNumber 紐付け元のPreスナップショット番号
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createPostSnapshot(const QString &description,
                                        int previousNumber,
                                        const QString &cleanup,
                                        bool important,
                                        const QVariantMap &userdata = QVariantMap());

    /**
     * @brief 選択中のスナップショットから、Postを作成できるPreスナップショットの番号を求める
     * @param selectedNumbers 選択中のスナップショット番号の一覧
     * @return 選択がPreスナップショット1件だけで、そのPreに対応するPostが一覧にまだ無い場合はPreの番号、それ以外は-1
     */
    Q_INVOKABLE int pendingPreNumber(const QVariantList &selectedNumbers) const;

    /**
     * @brief 指定番号へロールバックする
     * @param number ロールバック先のスナップショット番号
     */
    Q_INVOKABLE void rollbackSnapshot(int number);

    /**
     * @brief 単一スナップショットを削除する
     * @param number 削除するスナップショット番号
     */
    Q_INVOKABLE void deleteSnapshot(int number);

    /**
     * @brief 複数スナップショットを一括削除する
     * @param numbers 削除するスナップショット番号の一覧
     * @note 複数件の場合は、認証を1回で済ませるため、番号ごとに削除せずDeleteSnapshotsを1回だけ呼び出す
     *       1件だけの場合は、DeleteSnapshotを呼び出す
     *       DeleteSnapshotは、polkitが認可を一時保持する間は再認証を求めない
     *       1未満の番号と重複した番号は、D-Busを呼び出す前に除外する
     */
    Q_INVOKABLE void deleteSnapshots(const QVariantList &numbers);

    /**
     * @brief スナップショット情報を変更する
     * @param number 変更対象のスナップショット番号
     * @param description 新しい説明文
     * @param cleanup 新しいクリーンアップアルゴリズム文字列
     * @param userdata 新しいユーザデータマップ
     */
    Q_INVOKABLE void modifySnapshot(int number,
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata);

signals:
    /**
     * @brief 件数が変更されたときに発行する
     */
    void countChanged();

    /**
     * @brief スナップショット作成成功時に発行する
     */
    void snapshotCreated();

    /**
     * @brief スナップショット作成失敗時に発行する
     * @param error 失敗理由
     */
    void snapshotCreationFailed(const QString &error);

    /**
     * @brief ロールバック完了時に発行する
     */
    void rollbackCompleted();

    /**
     * @brief ロールバック失敗時に発行する
     * @param error 失敗理由
     */
    void rollbackFailed(const QString &error);

    /**
     * @brief 単一削除成功時に発行する
     * @param number 削除されたスナップショット番号
     */
    void snapshotDeleted(int number);

    /**
     * @brief 単一削除失敗時に発行する
     * @param number 削除対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotDeletionFailed(int number, const QString &error);

    /**
     * @brief 複数削除完了時に発行する
     * @param successCount 削除を確認できた件数
     * @param failureCount 削除を確認できなかった件数
     * @note failureCountが0より大きい場合は一部失敗であり、成功として扱ってはならない
     */
    void snapshotsDeletionCompleted(int successCount, int failureCount);

    /**
     * @brief 複数削除を開始できなかった、または応答がエラーのときに発行する
     * @param error 利用者へ表示する失敗理由
     * @note この場合はsnapshotsDeletionCompletedを発行しない (成功件数を知らせる手段がないため)
     */
    void snapshotsDeletionFailed(const QString &error);

    /**
     * @brief 変更成功時に発行する
     * @param number 変更されたスナップショット番号
     */
    void snapshotModified(int number);

    /**
     * @brief 変更失敗時に発行する
     * @param number 変更対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotModificationFailed(int number, const QString &error);

private slots:
    /**
     * @brief 作成成功通知を処理する
     * @param snapshot 作成されたスナップショット
     */
    void onSnapshotCreated(FsSnapshot *snapshot);

    /**
     * @brief 作成失敗通知を処理する
     * @param error 失敗理由
     */
    void onSnapshotCreationFailed(const QString &error);

    /**
     * @brief ロールバック完了通知を処理する
     */
    void onRollbackCompleted();

    /**
     * @brief ロールバック失敗通知を処理する
     * @param error 失敗理由
     */
    void onRollbackFailed(const QString &error);

    /**
     * @brief 削除成功通知を処理する
     * @param number 削除されたスナップショット番号
     */
    void onSnapshotDeleted(int number);

    /**
     * @brief 削除失敗通知を処理する
     * @param number 削除対象のスナップショット番号
     * @param error 失敗理由
     */
    void onSnapshotDeletionFailed(int number, const QString &error);

private:
    // モデルデータ
    QList<FsSnapshot*> m_snapshots;     // スナップショットオブジェクトのリスト
    SnapperService *m_snapperService;   // SnapperServiceシングルトンインスタンスへのポインタ
    quint64 m_deletionGeneration;       // 一括削除の世代番号 (古い応答を捨てるため)
};

#endif // SNAPSHOTLISTMODEL_H
