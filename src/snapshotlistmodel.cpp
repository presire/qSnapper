#include "snapshotlistmodel.h"
#include "snapperservice.h"
#include <QDebug>

/**
 * @brief SnapshotListModelオブジェクトを構築
 *
 * スナップショット一覧をQMLで表示するためのリストモデルを構築する。
 * SnapperServiceのシグナルを接続し、スナップショット操作の結果を
 * モデルに反映する。
 *
 * @param parent 親QObjectポインタ
 */
SnapshotListModel::SnapshotListModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_snapperService(SnapperService::instance())
{
    connect(m_snapperService, &SnapperService::snapshotCreated,
            this, &SnapshotListModel::onSnapshotCreated);
    connect(m_snapperService, &SnapperService::snapshotCreationFailed,
            this, &SnapshotListModel::onSnapshotCreationFailed);
    connect(m_snapperService, &SnapperService::rollbackCompleted,
            this, &SnapshotListModel::onRollbackCompleted);
    connect(m_snapperService, &SnapperService::rollbackFailed,
            this, &SnapshotListModel::onRollbackFailed);
    connect(m_snapperService, &SnapperService::snapshotDeleted,
            this, &SnapshotListModel::onSnapshotDeleted);
    connect(m_snapperService, &SnapperService::snapshotDeletionFailed,
            this, &SnapshotListModel::onSnapshotDeletionFailed);
}

/**
 * @brief SnapshotListModelオブジェクトを破棄
 *
 * 保持している全てのFsSnapshotオブジェクトのメモリを解放する。
 */
SnapshotListModel::~SnapshotListModel()
{
    qDeleteAll(m_snapshots);
}

/**
 * @brief モデルの行数を取得
 *
 * QMLのListViewで表示するスナップショット数を返す。
 * Qt Model/Viewフレームワークの必須実装メソッド。
 *
 * @param parent 親インデックス (リストモデルでは常に無効)
 *
 * @return スナップショットの総数
 */
int SnapshotListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return m_snapshots.count();
}

/**
 * @brief 指定されたインデックスとロールのデータを取得
 *
 * QMLのListViewデリゲートがアクセスするスナップショットデータを返す。
 * ロールに応じて、番号、タイプ、日時、説明などの情報を提供する。
 * Qt Model/Viewフレームワークの必須実装メソッド。
 *
 * @param index データを取得するモデルインデックス
 * @param role 取得するデータのロール (NumberRole, DescriptionRoleなど)
 *
 * @return 指定されたロールのデータ、無効なインデックスの場合は空のQVariant
 */
QVariant SnapshotListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_snapshots.count())
        return QVariant();

    FsSnapshot *snapshot = m_snapshots.at(index.row());

    switch (role) {
    case NumberRole:
        return snapshot->number();
    case SnapshotTypeRole:
        return static_cast<int>(snapshot->snapshotType());
    case PreviousNumberRole:
        return snapshot->previousNumber();
    case TimestampRole:
        return snapshot->timestamp();
    case UserRole:
        return snapshot->user();
    case CleanupAlgoRole:
        return static_cast<int>(snapshot->cleanupAlgo());
    case DescriptionRole:
        return snapshot->description();
    case SnapshotTypeStringRole:
        return snapshot->snapshotTypeString();
    case CleanupAlgoStringRole:
        return snapshot->cleanupAlgoString();
    case UserdataRole:
        return snapshot->userdata();
    default:
        return QVariant();
    }
}

/**
 * @brief QMLで使用するロール名のマッピングを取得
 *
 * C++のロール列挙値とQMLでアクセス可能なプロパティ名のマッピングを定義する。
 * QMLからは "number", "description" などの名前でデータにアクセスできる。
 * Qt Model/Viewフレームワークの必須実装メソッド。
 *
 * @return ロールIDとロール名のハッシュマップ
 */
QHash<int, QByteArray> SnapshotListModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NumberRole] = "number";
    roles[SnapshotTypeRole] = "snapshotType";
    roles[PreviousNumberRole] = "previousNumber";
    roles[TimestampRole] = "timestamp";
    roles[UserRole] = "user";
    roles[CleanupAlgoRole] = "cleanupAlgo";
    roles[DescriptionRole] = "description";
    roles[SnapshotTypeStringRole] = "snapshotTypeString";
    roles[CleanupAlgoStringRole] = "cleanupAlgoString";
    roles[UserdataRole] = "userdata";
    return roles;
}

/**
 * @brief スナップショット一覧を再読み込み
 *
 * SnapperServiceから最新のスナップショット一覧を取得し、
 * モデルの内容を更新する。既存のスナップショットオブジェクトは
 * 全て削除され、新しいデータで置き換えられる。
 * QMLから呼び出し可能なメソッド。
 */
void SnapshotListModel::refresh()
{
    beginResetModel();
    qDeleteAll(m_snapshots);
    m_snapshots.clear();
    m_snapshots = m_snapperService->all();
    endResetModel();
    emit countChanged();
}

/**
 * @brief Singleタイプのスナップショットを作成
 *
 * 独立した単一のスナップショットを作成する。
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する。
 * 作成結果はシグナル経由で通知される。
 * QMLから呼び出し可能なメソッド。
 *
 * @param description スナップショットの説明文
 */
void SnapshotListModel::createSingleSnapshot(const QString &description)
{
    m_snapperService->createSingle(description);
}

/**
 * @brief Preタイプのスナップショットを作成
 *
 * 操作前の状態を記録するPreスナップショットを作成する。
 * 後でPostスナップショットとペアにして差分管理に使用できる。
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する。
 * 作成結果はシグナル経由で通知される。
 * QMLから呼び出し可能なメソッド。
 *
 * @param description スナップショットの説明文
 */
void SnapshotListModel::createPreSnapshot(const QString &description)
{
    m_snapperService->createPre(description);
}

/**
 * @brief Postタイプのスナップショットを作成
 *
 * 操作後の状態を記録し、指定されたPreスナップショットとペアにするPostスナップショットを作成する。
 * PreとPostのペアにより、操作前後の差分を管理できる。
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する。
 * 作成結果はシグナル経由で通知される。
 * QMLから呼び出し可能なメソッド。
 *
 * @param description スナップショットの説明文
 * @param previousNumber ペアにするPreスナップショットの番号
 */
void SnapshotListModel::createPostSnapshot(const QString &description, int previousNumber)
{
    m_snapperService->createPost(description, previousNumber);
}

/**
 * @brief 指定されたスナップショットにシステムをロールバック
 *
 * システム全体を指定されたスナップショットの状態に戻す。
 * この操作は破壊的で、ロールバック後は再起動が必要となる。
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する。
 * 実行結果はシグナル経由で通知される。
 * QMLから呼び出し可能なメソッド。
 *
 * @param number ロールバック先のスナップショット番号
 */
void SnapshotListModel::rollbackSnapshot(int number)
{
    m_snapperService->rollback(number);
}

/**
 * @brief 指定されたスナップショットを削除
 *
 * スナップショット番号を指定して単一のスナップショットを削除する。
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する。
 * 削除結果はシグナル経由で通知される。
 * QMLから呼び出し可能なメソッド。
 *
 * @param number 削除するスナップショット番号
 */
void SnapshotListModel::deleteSnapshot(int number)
{
    m_snapperService->deleteSnapshot(number);
}

/**
 * @brief 複数のスナップショットを一括削除
 *
 * スナップショット番号のリストを受け取り、各スナップショットを順次削除する。
 * 成功数と失敗数をカウントし、全ての削除処理完了後にシグナルで通知する。
 * 処理完了後、スナップショット一覧を自動的に再読み込みする。
 * QMLから呼び出し可能なメソッド。
 *
 * @param numbers 削除するスナップショット番号のリスト (QVariant配列)
 */
void SnapshotListModel::deleteSnapshots(const QVariantList &numbers)
{
    int successCount = 0;
    int failureCount = 0;

    for (const QVariant &numVariant : numbers) {
        bool ok;
        int number = numVariant.toInt(&ok);
        if (!ok) {
            failureCount++;
            continue;
        }

        bool success = m_snapperService->deleteSnapshot(number);
        if (success) {
            successCount++;
        } else {
            failureCount++;
        }
    }

    refresh();
    emit snapshotsDeletionCompleted(successCount, failureCount);
}

/**
 * @brief スナップショット作成成功時の内部ハンドラ
 *
 * SnapperServiceからのスナップショット作成成功シグナルを受け取り、
 * モデルを更新してQMLにシグナルを転送する。
 *
 * @param snapshot 作成されたスナップショットオブジェクト (未使用)
 */
void SnapshotListModel::onSnapshotCreated(FsSnapshot *snapshot)
{
    Q_UNUSED(snapshot);
    refresh();
    emit snapshotCreated();
}

/**
 * @brief スナップショット作成失敗時の内部ハンドラ
 *
 * SnapperServiceからのスナップショット作成失敗シグナルを受け取り、
 * エラーメッセージをQMLに転送する。
 *
 * @param error エラーメッセージ
 */
void SnapshotListModel::onSnapshotCreationFailed(const QString &error)
{
    emit snapshotCreationFailed(error);
}

/**
 * @brief ロールバック成功時の内部ハンドラ
 *
 * SnapperServiceからのロールバック成功シグナルを受け取り、
 * モデルを更新してQMLにシグナルを転送する。
 */
void SnapshotListModel::onRollbackCompleted()
{
    refresh();
    emit rollbackCompleted();
}

/**
 * @brief ロールバック失敗時の内部ハンドラ
 *
 * SnapperServiceからのロールバック失敗シグナルを受け取り、
 * エラーメッセージをQMLに転送する。
 *
 * @param error エラーメッセージ
 */
void SnapshotListModel::onRollbackFailed(const QString &error)
{
    emit rollbackFailed(error);
}

/**
 * @brief スナップショット削除成功時の内部ハンドラ
 *
 * SnapperServiceからのスナップショット削除成功シグナルを受け取り、
 * 削除されたスナップショット番号をQMLに転送する。
 *
 * @param number 削除されたスナップショット番号
 */
void SnapshotListModel::onSnapshotDeleted(int number)
{
    emit snapshotDeleted(number);
}

/**
 * @brief スナップショット削除失敗時の内部ハンドラ
 *
 * SnapperServiceからのスナップショット削除失敗シグナルを受け取り、
 * 失敗したスナップショット番号とエラーメッセージをQMLに転送する。
 *
 * @param number 削除に失敗したスナップショット番号
 * @param error エラーメッセージ
 */
void SnapshotListModel::onSnapshotDeletionFailed(int number, const QString &error)
{
    emit snapshotDeletionFailed(number, error);
}
