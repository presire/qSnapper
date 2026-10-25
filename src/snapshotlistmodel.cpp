/**
 * @file snapshotlistmodel.cpp
 * @brief スナップショット一覧モデルの実装
 */

#include "snapshotlistmodel.h"
#include "snapperservice.h"
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>

/**
 * @brief SnapshotListModelオブジェクトを構築
 *
 * スナップショット一覧をQMLで表示するためのリストモデルを構築する
 * SnapperServiceのシグナルを接続し、スナップショット操作の結果をモデルに反映する
 *
 * @param parent 親QObjectポインタ
 */
SnapshotListModel::SnapshotListModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_snapperService(SnapperService::instance())
    , m_deletionGeneration(0)
{
    connect(m_snapperService, &SnapperService::snapshotCreated, this, &SnapshotListModel::onSnapshotCreated);
    connect(m_snapperService, &SnapperService::snapshotCreationFailed, this, &SnapshotListModel::onSnapshotCreationFailed);
    connect(m_snapperService, &SnapperService::rollbackCompleted, this, &SnapshotListModel::onRollbackCompleted);
    connect(m_snapperService, &SnapperService::rollbackFailed, this, &SnapshotListModel::onRollbackFailed);
    connect(m_snapperService, &SnapperService::snapshotDeleted, this, &SnapshotListModel::onSnapshotDeleted);
    connect(m_snapperService, &SnapperService::snapshotDeletionFailed, this, &SnapshotListModel::onSnapshotDeletionFailed);
}

/**
 * @brief SnapshotListModelオブジェクトを破棄
 *
 * 保持している全てのFsSnapshotオブジェクトのメモリを解放する
 */
SnapshotListModel::~SnapshotListModel()
{
    qDeleteAll(m_snapshots);
}

/**
 * @brief モデルの行数を取得
 *
 * QMLのListViewで表示するスナップショット数を返す
 * Qt Model/Viewフレームワークの必須実装メソッド
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
 * QMLのListViewデリゲートがアクセスするスナップショットデータを返す
 * ロールに応じて、番号、タイプ、日時、説明などの情報を提供する
 * Qt Model/Viewフレームワークの必須実装メソッド
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
 * C++のロール列挙値とQMLでアクセス可能なプロパティ名のマッピングを定義する
 * QMLからは "number", "description" などの名前でデータにアクセスできる
 * Qt Model/Viewフレームワークの必須実装メソッド
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
 * SnapperServiceから最新のスナップショット一覧を取得し、モデルの内容を更新する
 * 既存のスナップショットオブジェクトは全て削除され、新しいデータで置き換えられる
 *
 * QMLから呼び出し可能なメソッド
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
 * 独立した単一のスナップショットを作成する
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する
 * 作成結果はシグナル経由で通知される
 *
 * QMLから呼び出し可能なメソッド
 *
 * @param description スナップショットの説明文
 * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は""扱い)
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 */
void SnapshotListModel::createSingleSnapshot(const QString &description,
                                             const QString &cleanup,
                                             bool important,
                                             const QVariantMap &userdata)
{
    m_snapperService->createSingle(description, FsSnapshot::stringToCleanupAlgorithm(cleanup), important, userdata);
}

/**
 * @brief Preタイプのスナップショットを作成
 *
 * 操作前の状態を記録するPreスナップショットを作成する
 * 後でPostスナップショットとペアにして差分管理に使用できる
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する
 * 作成結果はシグナル経由で通知される
 *
 * QMLから呼び出し可能なメソッド
 *
 * @param description スナップショットの説明文
 * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は""扱い)
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 */
void SnapshotListModel::createPreSnapshot(const QString &description,
                                          const QString &cleanup,
                                          bool important,
                                          const QVariantMap &userdata)
{
    m_snapperService->createPre(description, FsSnapshot::stringToCleanupAlgorithm(cleanup), important, userdata);
}

/**
 * @brief Postタイプのスナップショットを作成
 *
 * 操作後の状態を記録し、指定されたPreスナップショットとペアにするPostスナップショットを作成する
 * PreとPostのペアにより、操作前後の差分を管理できる
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する
 * 作成結果はシグナル経由で通知される
 *
 * QMLから呼び出し可能なメソッド
 *
 * @param description スナップショットの説明文
 * @param previousNumber ペアにするPreスナップショットの番号
 * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は""扱い)
 * @param important 重要フラグ
 * @param userdata ユーザデータマップ
 */
void SnapshotListModel::createPostSnapshot(const QString &description,
                                           int previousNumber,
                                           const QString &cleanup,
                                           bool important,
                                           const QVariantMap &userdata)
{
    m_snapperService->createPost(description, previousNumber, FsSnapshot::stringToCleanupAlgorithm(cleanup),
                                 important, userdata);
}

/**
 * @brief 選択中のスナップショットから、Postを作成できるPreスナップショットの番号を求める
 *
 * Postの作成ダイアログで、Post種別を選べるかどうかと紐付け先のPre番号を決めるために使う
 * 選択が複数、またはPre以外、またはPostが既にあるPreの場合は、紐付け先を一意に決められないため-1を返す
 * (Postは1つのPreに1件だけ対応するため、既にPostがあるPreへ2件目を作らせない)
 *
 * QMLから呼び出し可能なメソッド
 *
 * @param selectedNumbers 選択中のスナップショット番号の一覧
 * @return Postが未作成のPreスナップショット1件だけを選択している場合はその番号、それ以外は-1
 */
int SnapshotListModel::pendingPreNumber(const QVariantList &selectedNumbers) const
{
    if (selectedNumbers.size() != 1) {
        return -1;
    }

    bool ok = false;
    const int number = selectedNumbers.first().toInt(&ok);
    if (!ok) {
        return -1;
    }

    const FsSnapshot *pre = nullptr;
    for (const FsSnapshot *snapshot : std::as_const(m_snapshots)) {
        if (snapshot->number() == number) {
            pre = snapshot;
            break;
        }
    }
    if (!pre || pre->snapshotType() != FsSnapshot::SnapshotType::Pre) {
        return -1;
    }

    for (const FsSnapshot *snapshot : std::as_const(m_snapshots)) {
        if (snapshot->snapshotType() == FsSnapshot::SnapshotType::Post
            && snapshot->previousNumber() == number) {
            return -1;
        }
    }
    return number;
}

/**
 * @brief 既存スナップショットを編集する
 *
 * 成功時はモデルを更新してsnapshotModified()を、
 * 失敗時はsnapshotModificationFailed()を発行する
 *
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文
 * @param cleanup 新しいクリーンアップアルゴリズム文字列
 * @param userdata 新しいユーザデータマップ
 */
void SnapshotListModel::modifySnapshot(int number,
                                       const QString &description,
                                       const QString &cleanup,
                                       const QVariantMap &userdata)
{
    bool success = m_snapperService->modifySnapshot(number, description, cleanup, userdata);
    if (success) {
        refresh();
        emit snapshotModified(number);
    }
    else {
        emit snapshotModificationFailed(number, tr("Failed to modify snapshot #%1").arg(number));
    }
}

/**
 * @brief 指定されたスナップショットにシステムをロールバック
 *
 * システム全体を指定されたスナップショットの状態に戻す
 * この操作は破壊的で、ロールバック後は再起動が必要となる
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する
 * 実行結果はシグナル経由で通知される
 *
 * QMLから呼び出し可能なメソッド
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
 * スナップショット番号を指定して単一のスナップショットを削除する
 * SnapperServiceを通じてD-Busサービスにリクエストを送信する
 * 削除結果はシグナル経由で通知される
 *
 * QMLから呼び出し可能なメソッド
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
 * 複数件の場合は、ヘルパのDeleteSnapshotsメソッドを1回だけ呼び出して削除する
 * 応答には削除を確認できた番号の一覧が返るため、その件数から成功数と失敗数を求める
 * 1件だけの場合は、ヘルパのDeleteSnapshotメソッドで削除し、応答の真偽値から成功数と失敗数を求める
 * どちらの場合も、応答を受け取った後にスナップショット一覧を再読み込みする
 *
 * QMLから呼び出し可能なメソッド
 *
 * @param numbers 削除するスナップショット番号のリスト (QVariant配列)
 * @note 削除の結果は、snapshotsDeletionCompletedシグナル (成功数と失敗数) で通知する
 *       エラー応答を受け取った場合や、有効な番号が1つもない場合は、snapshotsDeletionFailedシグナルで通知する
 *       番号ごとの同期呼び出しでは、GUIが固まり、認証も番号ごとに求められるため、非同期の呼び出しを使う
 *       削除の要求ごとに世代番号 (m_deletionGeneration) を進める
 *       新しい要求の開始後に届いた古い要求の応答は、世代番号が一致しないため無視する
 */
void SnapshotListModel::deleteSnapshots(const QVariantList &numbers)
{
    QList<uint> requested;
    for (const QVariant &value : numbers) {
        bool ok = false;
        const uint number = value.toUInt(&ok);
        if (!ok || number < 1) {
            continue;
        }
        if (!requested.contains(number)) {
            requested.append(number);
        }
    }

    if (requested.isEmpty()) {
        emit snapshotsDeletionFailed(tr("No valid snapshot numbers were selected."));
        return;
    }

    const quint64 generation = ++m_deletionGeneration;
    const int requestedCount = requested.size();
    const QString configName = m_snapperService->currentConfig();

    // 1件だけの削除には、DeleteSnapshotを使う
    // DeleteSnapshotは、polkitが認可を一時保持する間は再認証を求めず、snapperdを経由せずに削除する
    // DeleteSnapshotsは、毎回認証を求め、snapperdを経由するため作成直後のスナップショットをSnapshotListOutdatedで拒否する
    // そのため、DeleteSnapshotsは複数件をまとめて削除する場合だけに使う
    const bool single = (requestedCount == 1);
    const QDBusPendingCall call = single
        ? m_snapperService->asyncDeleteSnapshot(configName, requested.first())
        : m_snapperService->asyncDeleteSnapshots(configName, requested);

    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation, requestedCount, single]() {
        watcher->deleteLater();

        // より新しい一括削除が始まっている場合は、古い応答で表示を乱さない
        if (generation != m_deletionGeneration) {
            return;
        }

        if (watcher->isError()) {
            emit snapshotsDeletionFailed(SnapperService::errorText(watcher->error()));
        }
        else if (single) {
            // DeleteSnapshotは通常、失敗をエラー応答で返す
            // 正常な応答でfalseが返った場合も、削除できなかったものとして失敗数に数える
            QDBusPendingReply<bool> reply = *watcher;
            const int successCount = reply.value() ? 1 : 0;
            emit snapshotsDeletionCompleted(successCount, requestedCount - successCount);
        }
        else {
            QDBusPendingReply<QList<uint>> reply = *watcher;
            const int successCount = qMin(reply.value().size(), requestedCount);
            emit snapshotsDeletionCompleted(successCount, requestedCount - successCount);
        }

        // 一部失敗でも実際の状態を表示できるよう、結果にかかわらず一覧を読み直す
        refresh();
    });
}

/**
 * @brief スナップショット作成成功時の内部ハンドラ
 *
 * SnapperServiceからのスナップショット作成成功シグナルを受け取り、モデルを更新してQMLにシグナルを転送する
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
 * SnapperServiceからのスナップショット作成失敗シグナルを受け取り、エラーメッセージをQMLに転送する
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
 * SnapperServiceからのロールバック成功シグナルを受け取り、モデルを更新してQMLにシグナルを転送する
 */
void SnapshotListModel::onRollbackCompleted()
{
    refresh();
    emit rollbackCompleted();
}

/**
 * @brief ロールバック失敗時の内部ハンドラ
 *
 * SnapperServiceからのロールバック失敗シグナルを受け取り、エラーメッセージをQMLに転送する
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
 * SnapperServiceからのスナップショット削除成功シグナルを受け取り、削除されたスナップショット番号をQMLに転送する
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
 * SnapperServiceからのスナップショット削除失敗シグナルを受け取り、失敗したスナップショット番号とエラーメッセージをQMLに転送する
 *
 * @param number 削除に失敗したスナップショット番号
 * @param error エラーメッセージ
 */
void SnapshotListModel::onSnapshotDeletionFailed(int number, const QString &error)
{
    emit snapshotDeletionFailed(number, error);
}
