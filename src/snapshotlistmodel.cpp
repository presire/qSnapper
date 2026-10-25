/**
 * @file snapshotlistmodel.cpp
 * @brief スナップショット一覧を取得し、QMLからの操作をSnapperServiceへ中継するリストモデルの実装
 *
 * SnapperServiceのシグナルを接続し、取得した一覧を表形式のモデルとして公開する
 * 一覧の取得と各操作は非同期で行い、古い応答は世代番号で破棄する
 *
 * 利用する側 : SnapshotListModelクラスを生成するQML
 * 出力 : モデルのロール、count、loading、busy、各操作の完了シグナル
 * 依存 : SnapperService、qsnapper::userdata::parse、Qt D-Bus
 */

#include "snapshotlistmodel.h"
#include "snapperservice.h"
#include "userdataparser.h"
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
    , m_refreshGeneration(0)
    , m_loading(false)
{
    connect(m_snapperService, &SnapperService::snapshotCreated, this, &SnapshotListModel::onSnapshotCreated);
    connect(m_snapperService, &SnapperService::snapshotCreationFailed, this, &SnapshotListModel::onSnapshotCreationFailed);
    connect(m_snapperService, &SnapperService::rollbackCompleted, this, &SnapshotListModel::onRollbackCompleted);
    connect(m_snapperService, &SnapperService::rollbackFailed, this, &SnapshotListModel::onRollbackFailed);
    connect(m_snapperService, &SnapperService::snapshotDeleted, this, &SnapshotListModel::onSnapshotDeleted);
    connect(m_snapperService, &SnapperService::snapshotDeletionFailed, this, &SnapshotListModel::onSnapshotDeletionFailed);
    // 変更は一覧の再取得を伴うため、SnapperServiceからの完了通知を内部ハンドラで受ける
    connect(m_snapperService, &SnapperService::snapshotModified, this, &SnapshotListModel::onSnapshotModified);
    // 変更失敗は一覧を変えないが、QMLの編集ダイアログへ理由を返す必要がある
    connect(m_snapperService, &SnapperService::snapshotModificationFailed, this, &SnapshotListModel::onSnapshotModificationFailed);
    // 作成・変更・削除・復元の進行状況を、モデルのbusyプロパティとしてQMLへ公開する
    connect(m_snapperService, &SnapperService::operationBusyChanged, this, &SnapshotListModel::busyChanged);
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
 * @brief 処理中かどうかを取得
 *
 * @return SnapperServiceが作成・変更・削除・ロールバックのいずれかを処理中の場合はtrue
 */
bool SnapshotListModel::busy() const
{
    return m_snapperService->operationBusy();
}

/**
 * @brief スナップショット一覧を非同期に再読み込みする
 *
 * SnapperServiceへ最新のスナップショット一覧を要求し、応答を受け取ってからモデルの内容を置き換える
 * 既存のスナップショットオブジェクトは全て削除され、新しいデータで置き換えられる
 *
 * QMLから呼び出し可能なメソッド
 *
 * @note 同期呼び出しでは、snapperdの応答待ちや認証ダイアログの表示中にGUIが固まるため、応答はQDBusPendingCallWatcherで受け取る
 *       要求ごとに世代番号 (m_refreshGeneration) を進め、最新の要求の応答だけを反映する
 *       古い応答を反映すると、設定の切り替え直後に前の設定の一覧が表示されるためである
 *       エラー応答の場合は一覧を空にし、取得に失敗した内容を現在の一覧として表示しない
 *       空の一覧だけでは失敗と0件を区別できないため、listLoadFailedシグナルで失敗理由を画面へ伝える
 *       configを切り替えた場合は、応答を待たずに一覧を空にし、取得元のconfig (m_loadedConfig) を応答の反映時に記録する
 *       変更・削除・ロールバックは、取得元のconfigが現在のconfigと一致する場合だけ受け付ける
 */
void SnapshotListModel::refresh()
{
    const quint64 generation = ++m_refreshGeneration;
    if (!m_loading) {
        m_loading = true;
        emit loadingChanged();
    }

    // configを切り替えた場合は、応答を待つ間に前のconfigの行を選んで操作できないよう、一覧を先に空にする
    const QString requestedConfig = m_snapperService->currentConfig();
    if (requestedConfig != m_loadedConfig) {
        m_loadedConfig.clear();
        if (!m_snapshots.isEmpty()) {
            beginResetModel();
            qDeleteAll(m_snapshots);
            m_snapshots.clear();
            endResetModel();
            emit countChanged();
        }
    }

    auto *watcher = new QDBusPendingCallWatcher(m_snapperService->asyncListSnapshots(requestedConfig), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, requestedConfig]() {
        watcher->deleteLater();
        if (generation != m_refreshGeneration) {
            return;
        }

        QList<FsSnapshot*> snapshots;
        const bool loadFailed = watcher->isError();  // 一覧の取得に失敗したかどうか
        QString loadError;                           // 利用者へ表示する失敗理由 (成功時は空)
        if (loadFailed) {
            qWarning() << "Failed to list snapshots via D-Bus:" << watcher->error().message();
            loadError = SnapperService::errorText(watcher->error());

            // エラー本文が空の応答でも失敗を通知し、取得の失敗を0件の一覧と区別できるようにする
            if (loadError.isEmpty()) {
                loadError = tr("The snapshot list could not be retrieved.");
            }
        }
        else {
            const QDBusPendingReply<QString> reply = *watcher;
            snapshots = SnapperService::parseSnapshotList(reply.value());
        }

        beginResetModel();
        qDeleteAll(m_snapshots);
        m_snapshots = snapshots;
        endResetModel();
        m_loadedConfig = requestedConfig;
        emit countChanged();

        m_loading = false;
        emit loadingChanged();

        // 取得中の表示 (loading) を終えてから失敗を通知し、エラーダイアログの表示中に一覧が取得中のままに見えないようにする
        if (loadFailed) {
            emit listLoadFailed(loadError);
        }
    });
}

/**
 * @brief 表示中の一覧が、現在のconfigから取得し終えたものかを判定する
 * @return 取得が完了しており、取得元のconfigが現在のconfigと同じ場合true
 * @note 番号はconfigごとに振られるため、別のconfigの一覧の番号で操作すると、同じ番号の別のスナップショットを変更・削除してしまう
 */
bool SnapshotListModel::isListForCurrentConfig() const
{
    return !m_loading && m_loadedConfig == m_snapperService->currentConfig();
}

/**
 * @brief 一覧が現在のconfigのものではない、または取得中のため操作を受け付けない旨の文言を返す
 * @return 翻訳済みの文言
 */
QString SnapshotListModel::listOutdatedText() const
{
    return tr("The snapshot list is being reloaded. Try again after the list has been updated.");
}

/**
 * @brief 指定番号のスナップショットの編集用の情報を取得
 *
 * 編集ダイアログの初期値を、取得済みの一覧から求める
 *
 * @param number スナップショット番号
 * @return number、description、cleanup、userdataを持つマップ、一覧に無い場合は空のマップ
 * @note 一覧は取得済みのため、編集ダイアログを開くたびにD-Busで同期取得してGUIを止めないようキャッシュを参照する
 */
QVariantMap SnapshotListModel::snapshotInfo(int number) const
{
    for (const FsSnapshot *snapshot : std::as_const(m_snapshots)) {
        if (snapshot->number() != number) {
            continue;
        }

        QVariantMap info;
        info.insert(QStringLiteral("number"), snapshot->number());
        info.insert(QStringLiteral("description"), snapshot->description());
        info.insert(QStringLiteral("cleanup"), snapshot->cleanupAlgoString());
        info.insert(QStringLiteral("userdata"), snapshot->userdata());
        return info;
    }
    return QVariantMap();
}

/**
 * @brief ユーザデータの入力文字列を検証し、キーと値のマップへ変換
 *
 * 作成ダイアログと編集ダイアログの入力欄が、同じ規則で検証できるようにする
 *
 * @param text 改行区切りでkey=valueを並べた文字列
 * @return ok(成功したか)、userdata(キーと値のマップ)、error(失敗理由の翻訳済み文言)を持つマップ
 * @note 失敗した場合のuserdataは空である
 *       エラー文言に含めるキーは利用者が入力した文字列であり、表示側でプレーンテキストとして扱う
 */
QVariantMap SnapshotListModel::parseUserdata(const QString &text) const
{
    const qsnapper::userdata::ParseResult parsed = qsnapper::userdata::parse(text);

    QString error;
    switch (parsed.error) {
    case qsnapper::userdata::ParseError::None:
        break;
    case qsnapper::userdata::ParseError::MissingSeparator:
        error = tr("Line %1 of the user data is not in key=value format.").arg(parsed.line);
        break;
    case qsnapper::userdata::ParseError::EmptyKey:
        error = tr("Line %1 of the user data has an empty key.").arg(parsed.line);
        break;
    case qsnapper::userdata::ParseError::DuplicateKey:
        error = tr("The user data key \"%1\" is specified more than once (line %2).").arg(parsed.key).arg(parsed.line);
        break;
    }

    QVariantMap result;
    result.insert(QStringLiteral("ok"), parsed.ok);
    result.insert(QStringLiteral("userdata"), parsed.values);
    result.insert(QStringLiteral("error"), error);
    return result;
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
 * @brief 既存スナップショットを非同期に編集する
 *
 * SnapperServiceへ編集を要求する
 * 成功時は一覧を取得し直してsnapshotModified()を、
 * 失敗時はsnapshotModificationFailed()を発行する
 *
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文
 * @param cleanup 新しいクリーンアップアルゴリズム文字列
 * @param userdata 新しいユーザデータマップ
 * @note 処理中はbusyがtrueになる
 *       結果はSnapperServiceのシグナルを受けたonSnapshotModified / onSnapshotModificationFailedが転送する
 */
void SnapshotListModel::modifySnapshot(int number,
                                       const QString &description,
                                       const QString &cleanup,
                                       const QVariantMap &userdata)
{
    // 番号は表示中の一覧から選ばれるため、一覧が現在のconfigのものと確かめてから要求する
    if (!isListForCurrentConfig()) {
        emit snapshotModificationFailed(number, listOutdatedText());
        return;
    }
    m_snapperService->modifySnapshot(number, description, cleanup, userdata);
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
    // 別のconfigの一覧の番号でロールバックしないよう、一覧が現在のconfigのものと確かめてから要求する
    if (!isListForCurrentConfig()) {
        emit rollbackFailed(listOutdatedText());
        return;
    }
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
    // 別のconfigにある同じ番号のスナップショットを削除しないよう、一覧が現在のconfigのものと確かめてから要求する
    if (!isListForCurrentConfig()) {
        emit snapshotDeletionFailed(number, listOutdatedText());
        return;
    }
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

    // 別のconfigにある同じ番号のスナップショットを削除しないよう、一覧が現在のconfigのものと確かめてから要求する
    if (!isListForCurrentConfig()) {
        emit snapshotsDeletionFailed(listOutdatedText());
        return;
    }

    const quint64 generation = ++m_deletionGeneration;
    const int requestedCount = requested.size();
    const QString configName = m_snapperService->currentConfig();

    // 1件だけの削除には、DeleteSnapshotを使う
    // DeleteSnapshotは、Polkitが認可を一時保持する間は再認証を求めず、snapperdを経由せずに削除する
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
 * @brief スナップショット変更成功時の内部ハンドラ
 *
 * SnapperServiceからの変更成功シグナルを受け取り、変更内容を反映するため一覧を取得し直してQMLにシグナルを転送する
 *
 * @param number 変更されたスナップショット番号
 * @note 一覧の再取得は非同期のため、snapshotModifiedは再取得の完了前に発行される
 *       呼び出し側が一覧の更新を待つ必要がある場合は、loadingの変化を確認する必要がある
 */
void SnapshotListModel::onSnapshotModified(int number)
{
    refresh();
    emit snapshotModified(number);
}

/**
 * @brief スナップショット変更失敗時の内部ハンドラ
 *
 * SnapperServiceからの変更失敗シグナルを受け取り、エラーメッセージをQMLに転送する
 *
 * @param number 変更に失敗したスナップショット番号
 * @param error エラーメッセージ
 * @note 一覧は変更されていないため、再取得は行わない
 */
void SnapshotListModel::onSnapshotModificationFailed(int number, const QString &error)
{
    emit snapshotModificationFailed(number, error);
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
