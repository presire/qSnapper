#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QSharedPointer>
#include <algorithm>
#include "filechangemodel.h"

namespace {

/**
 * @brief 解析済みの変更レコードを保持する
 */
struct ChangeInfo
{
    QString path;
    FileChangeItem::ChangeType type;
    QString statusFlags;
    bool isDirectory;
};

/**
 * @brief レガシーの変更レコードからステータスとパスを抽出する
 *
 * 最初の空白区切りだけを消費するため、パス内の空白はそのまま保持する
 *
 * @param line 改行を除いた変更レコード
 * @param info 解析結果の格納先
 * @return 有効なレコードを解析できた場合はtrue
 */
bool parseChangeRecord(const QString &line, ChangeInfo *info)
{
    const int separatorStart = line.indexOf(QRegularExpression(QStringLiteral("\\s+")));
    if (separatorStart <= 0) {
        return false;
    }

    int pathStart = separatorStart;
    while (pathStart < line.size() && line.at(pathStart).isSpace()) {
        ++pathStart;
    }
    if (pathStart >= line.size()) {
        return false;
    }

    const QString filePath = line.mid(pathStart);
    info->statusFlags = line.left(separatorStart);
    info->path = filePath.endsWith('/') ? filePath.left(filePath.size() - 1) : filePath;
    info->isDirectory = filePath.endsWith('/');
    return !info->path.isEmpty();
}

/**
 * @brief パスの親ディレクトリを集合へ追加する
 *
 * @param path 正規化済みの絶対パス
 * @param parentPaths 親パスを格納する集合
 */
void collectParentPaths(const QString &path, QSet<QString> *parentPaths)
{
    int separator = path.indexOf('/', 1);
    while (separator > 0) {
        parentPaths->insert(path.left(separator));
        separator = path.indexOf('/', separator + 1);
    }
}

} // namespace

// ============================================================================
// RestorePlanTransport / SystemBusRestorePlanTransport Implementation
// ============================================================================

RestorePlanTransport::~RestorePlanTransport() = default;

/**
 * @brief RestorePlanTransportのsystem bus実装
 *
 * com.presire.qsnapper.Operations のstaged restoreメソッド群を非同期で呼び出す。
 * Polkit認証はcommitPlan()の呼び出し時にサーバ側で1度だけ行われるため、
 * beginPlan() / stageEntries() は認証なしで呼び出せる。
 */
class SystemBusRestorePlanTransport : public QObject, public RestorePlanTransport
{
public:
    /**
     * @brief SystemBusRestorePlanTransportを構築する
     *
     * @param parent 親QObject (未指定の場合は呼び出し側がdeleteする)
     */
    explicit SystemBusRestorePlanTransport(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /**
     * @brief staging計画の作成を要求する
     *
     * @param configName Snapper設定名
     * @param snapshotNumber スナップショット番号
     * @param restoreMode 復元方式 ("yast" または "direct")
     * @param done 結果コールバック (ok, manifestId, error)
     */
    void beginPlan(const QString &configName, int snapshotNumber, const QString &restoreMode,
                   std::function<void(bool ok, const QString &manifestId, const QString &error)> done) override
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "BeginRestorePlan"
        );
        message << configName << snapshotNumber << restoreMode;

        callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<QString> reply = *watcher;
            if (reply.isError()) {
                done(false, QString(), reply.error().message());
                return;
            }
            // 空のマニフェストIDはサーバ側での計画作成失敗を示す
            const QString manifestId = reply.value();
            done(!manifestId.isEmpty(), manifestId,
                 manifestId.isEmpty() ? QStringLiteral("Restore plan was rejected") : QString());
        });
    }

    /**
     * @brief staging計画へ検証済みエントリのチャンクを追加する
     *
     * @param manifestId 対象計画のマニフェストID
     * @param paths エントリのパスリスト
     * @param changeTypes パスに対応する変更タイプリスト
     * @param done 結果コールバック (ok, error)
     */
    void stageEntries(const QString &manifestId, const QStringList &paths, const QStringList &changeTypes,
                      std::function<void(bool ok, const QString &error)> done) override
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "StageRestoreEntries"
        );
        message << manifestId << paths << changeTypes;

        callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<bool> reply = *watcher;
            if (reply.isError()) {
                done(false, reply.error().message());
            }
            else if (!reply.value()) {
                done(false, QStringLiteral("Restore entries were rejected"));
            }
            else {
                done(true, QString());
            }
        });
    }

    /**
     * @brief 計画を凍結して認証と実行を開始させる
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     */
    void commitPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "CommitRestorePlan"
        );
        message << manifestId;

        callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<bool> reply = *watcher;
            if (reply.isError()) {
                done(false, reply.error().message());
            }
            else if (!reply.value()) {
                done(false, QStringLiteral("Restore plan commit was rejected"));
            }
            else {
                done(true, QString());
            }
        });
    }

    /**
     * @brief 計画のキャンセルを要求する
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     */
    void cancelPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "CancelRestorePlan"
        );
        message << manifestId;

        callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<bool> reply = *watcher;
            if (reply.isError()) {
                done(false, reply.error().message());
            }
            else if (!reply.value()) {
                done(false, QStringLiteral("Restore plan cancel was rejected"));
            }
            else {
                done(true, QString());
            }
        });
    }

    /**
     * @brief 計画の状態をCSV形式で問い合わせる
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, statusCsv, error)
     */
    void requestStatus(const QString &manifestId,
                       std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) override
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "GetRestorePlanStatus"
        );
        message << manifestId;

        callPlanMethod(message, [done](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<QString> reply = *watcher;
            if (reply.isError()) {
                done(false, QString(), reply.error().message());
                return;
            }
            // 空の返り値はサーバ側での問い合わせ失敗を示す
            const QString statusCsv = reply.value();
            done(!statusCsv.isEmpty(), statusCsv,
                 statusCsv.isEmpty() ? QStringLiteral("Restore plan status was rejected") : QString());
        });
    }

    /**
     * @brief 復元計画の進捗/完了シグナルとサービス消失通知を受信側に登録する
     *
     * @param receiver 復元計画スロットとonRestorePlanServiceVanishedスロットを持つQObject
     * @return すべてのシグナル登録に成功した場合はtrue
     */
    bool subscribePlanSignals(QObject *receiver) override
    {
        const bool progressOk = QDBusConnection::systemBus().connect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restorePlanProgress",
            receiver,
            SLOT(onRestorePlanProgress(QString,int,int,QString))
        );
        const bool finishedOk = QDBusConnection::systemBus().connect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restorePlanFinished",
            receiver,
            SLOT(onRestorePlanFinished(QString,QString,QString))
        );
        if (!m_serviceWatcher) {
            m_serviceWatcher = new QDBusServiceWatcher(
                QStringLiteral("com.presire.qsnapper.Operations"),
                QDBusConnection::systemBus(),
                QDBusServiceWatcher::WatchForUnregistration,
                this
            );
        }
        disconnect(m_serviceWatcher, SIGNAL(serviceUnregistered(QString)),
                   receiver, SLOT(onRestorePlanServiceVanished()));
        const bool watcherOk = connect(
            m_serviceWatcher,
            SIGNAL(serviceUnregistered(QString)),
            receiver,
            SLOT(onRestorePlanServiceVanished())
        );
        return progressOk && finishedOk && watcherOk;
    }

    /**
     * @brief 復元計画の進捗/完了シグナルとサービス消失通知の受信を解除する
     *
     * @param receiver 登録解除するQObject
     */
    void unsubscribePlanSignals(QObject *receiver) override
    {
        QDBusConnection::systemBus().disconnect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restorePlanProgress",
            receiver,
            SLOT(onRestorePlanProgress(QString,int,int,QString))
        );
        QDBusConnection::systemBus().disconnect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restorePlanFinished",
            receiver,
            SLOT(onRestorePlanFinished(QString,QString,QString))
        );
        if (m_serviceWatcher) {
            disconnect(m_serviceWatcher, SIGNAL(serviceUnregistered(QString)),
                       receiver, SLOT(onRestorePlanServiceVanished()));
        }
    }

private:
    QDBusServiceWatcher *m_serviceWatcher = nullptr;

    /**
     * @brief 復元計画メソッドを非同期で呼び出す (タイムアウトなし)
     *
     * watcherはthisの子として管理されるため、transportの破棄時に未完了のコールバックが発火することはない
     *
     * @param message 送信するメソッドコール
     * @param handler watcherの完了時に呼び出すハンドラ
     */
    void callPlanMethod(const QDBusMessage &message,
                        std::function<void(QDBusPendingCallWatcher *watcher)> handler)
    {
        QDBusPendingCall pendingCall = QDBusConnection::systemBus().asyncCall(message, -1);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);

        connect(watcher, &QDBusPendingCallWatcher::finished, this, [handler, watcher](QDBusPendingCallWatcher *w) {
            Q_UNUSED(w);
            handler(watcher);
            watcher->deleteLater();
        });
    }
};

// ============================================================================
// FileChangeItem Implementation
// ============================================================================

/**
 * @brief FileChangeItemのコンストラクタ
 *
 * ファイル変更アイテムを作成し、パス、変更タイプ、親アイテムを設定する
 *
 * @param path ファイルパス
 * @param type 変更タイプ (Created, Deleted, Modifiedなど)
 * @param parent 親アイテムへのポインタ
 */
FileChangeItem::FileChangeItem(const QString &path, ChangeType type, const QString &statusFlags, FileChangeItem *parent)
    : m_path(path), m_changeType(type), m_statusFlags(statusFlags), m_parent(parent)
{
}

/**
 * @brief FileChangeItemのデストラクタ
 *
 * 全ての子アイテムを削除する
 */
FileChangeItem::~FileChangeItem()
{
    qDeleteAll(m_children);
}

/**
 * @brief 子アイテムを追加
 *
 * このアイテムに子アイテムを追加する
 *
 * @param child 追加する子アイテムへのポインタ
 */
void FileChangeItem::appendChild(FileChangeItem *child)
{
    m_children.append(child);
}

/**
 * @brief 指定された行番号の子アイテムを取得
 *
 * 指定された行番号に対応する子アイテムを返す
 *
 * @param row 子アイテムの行番号
 * @return 子アイテムへのポインタ (範囲外の場合はnullptr)
 */
FileChangeItem *FileChangeItem::child(int row)
{
    if (row < 0 || row >= m_children.size())
        return nullptr;
    return m_children.at(row);
}

/**
 * @brief 子アイテムの数を取得
 *
 * このアイテムが持つ子アイテムの数を返す
 *
 * @return 子アイテムの数
 */
int FileChangeItem::childCount() const
{
    return m_children.size();
}

/**
 * @brief 親アイテム内での行番号を取得
 *
 * このアイテムが親アイテムの何番目の子であるかを返す
 *
 * @return 行番号 (親がない場合は0)
 */
int FileChangeItem::row() const
{
    if (m_parent)
        return m_parent->m_children.indexOf(const_cast<FileChangeItem*>(this));
    return 0;
}

/**
 * @brief 親アイテムを取得
 *
 * このアイテムの親アイテムへのポインタを返す
 *
 * @return 親アイテムへのポインタ
 */
FileChangeItem *FileChangeItem::parent()
{
    return m_parent;
}

/**
 * @brief ファイル名またはディレクトリ名を取得
 *
 * パスからファイル名またはディレクトリ名を抽出して返す
 *
 * @return ファイル名またはディレクトリ名
 */
QString FileChangeItem::name() const
{
    if (m_path.isEmpty())
        return QString();

    // パスの末尾がスラッシュの場合は削除してから処理
    QString path = m_path;
    if (path.endsWith('/') && path.length() > 1) {
        path = path.left(path.length() - 1);
    }

    QFileInfo info(path);
    QString fileName = info.fileName();

    // ルートディレクトリの場合
    if (fileName.isEmpty() && path == "/") {
        return "/";
    }

    return fileName;
}

/**
 * @brief ディレクトリかどうかを判定
 *
 * パスの末尾がスラッシュで終わっているか、子要素があればディレクトリと判定する
 *
 * @return ディレクトリの場合: true、それ以外: false
 */
bool FileChangeItem::isDirectory() const
{
    // パスの末尾が/で終わっているか、子要素があればディレクトリ
    return m_path.endsWith('/') || !m_children.isEmpty();
}

// ============================================================================
// FileChangeModel Implementation
// ============================================================================

/**
 * @brief FileChangeModelのコンストラクタ
 *
 * モデルを初期化し、D-Busインターフェースへの接続を確立する
 *
 * @param parent 親QObjectへのポインタ
 */
FileChangeModel::FileChangeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_snapshotNumber(0)
    , m_compareNumber1(0)
    , m_compareNumber2(0)
    , m_betweenMode(false)
    , m_flatMode(false)
    , m_rootItem(nullptr)
    , m_dbusInterface(nullptr)
    , m_hasChanges(false)
    , m_loading(false)
    , m_defaultRestorePlanTransport(nullptr)
    , m_restorePlanTransport(nullptr)
    , m_planNextStageIndex(0)
    , m_planCommitted(false)
    , m_planActive(false)
    , m_planSignalsSubscribed(false)
    , m_planCancelRequested(false)
    , m_planTotalFiles(0)
    , m_planLastProgress(0)
    , m_totalFilesCount(0)
    , m_processedFilesCount(0)
    , m_restoreHasError(false)
    , m_cancelRequested(false)
    , m_restoreBatchSize(100)
    , m_useDirectRestore(true)
{
    // 復元設定をQSettingsから読み込み
    QSettings settings("Presire", "qSnapper");
    m_restoreBatchSize = qBound(1, settings.value("restore/batchSize", 100).toInt(), 1000);
    m_useDirectRestore = settings.value("restore/useDirectMethod", true).toBool();

    m_rootItem = new FileChangeItem("", FileChangeItem::Modified);

    m_dbusInterface = new QDBusInterface(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        QDBusConnection::systemBus(),
        this
    );

    if (!m_dbusInterface->isValid()) {
        qWarning() << "Failed to connect to D-Bus service:" << QDBusConnection::systemBus().lastError().message();
    }

    // 復元計画 (staged restore) 用のtransportを構築する
    m_defaultRestorePlanTransport = new SystemBusRestorePlanTransport();
    m_restorePlanTransport = m_defaultRestorePlanTransport;
}

/**
 * @brief D-Busサービスへの再接続を試みる
 *
 * アイドルタイムアウトでヘルパープロセスが終了した場合など、D-Busインターフェースが無効になった場合に呼び出す
 * QDBusInterfaceを再生成することでD-Bus activationが発動し、ヘルパープロセスが自動的に再起動される
 *
 * @return 再接続に成功した場合はtrue
 */
bool FileChangeModel::reconnectDbus()
{
    qWarning() << "D-Bus service lost, attempting to reconnect...";

    // startService()でヘルパーの起動完了を待ってから接続する
    // Qt 6では、startService()はQDBusReply<void>を返すため、isValid()のみ確認する
    auto startReply = QDBusConnection::systemBus().interface()->startService(
        "com.presire.qsnapper.Operations");
    if (!startReply.isValid()) {
        qWarning() << "Failed to start D-Bus service:"
                   << startReply.error().message();
        return false;
    }

    delete m_dbusInterface;
    m_dbusInterface = new QDBusInterface(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        QDBusConnection::systemBus(),
        this
    );

    if (!m_dbusInterface->isValid()) {
        qWarning() << "Reconnection failed:"
                   << QDBusConnection::systemBus().lastError().message();
        return false;
    }

    qInfo() << "Reconnected to D-Bus service successfully.";
    return true;
}

/**
 * @brief FileChangeModelのデストラクタ
 *
 * ルートアイテムとその配下の全てのアイテムを削除する
 */
FileChangeModel::~FileChangeModel()
{
    delete m_rootItem;
    // 既定のtransportのみ所有して破棄する (テスト注入分は所有権なし)
    delete m_defaultRestorePlanTransport;
}

/**
 * @brief 復元進捗のスロット
 *
 * D-Busから送信される復元進捗シグナルを受信し、全体の進捗を計算してemitする
 *
 * @param current バッチ内の現在処理中のファイル数
 * @param total バッチ内の総ファイル数
 * @param filePath 現在処理中のファイルパス
 */
void FileChangeModel::onRestoreProgress(int current, int total, const QString &filePath)
{
    // バッチ内の進捗を全体の進捗に変換
    // currentとtotalはバッチ内の進捗ではなく、UndoStepsの進捗
    int overallCurrent = m_processedFilesCount + current;
    int overallTotal = m_totalFilesCount;

    emit restoreProgress(overallCurrent, overallTotal, filePath);
}

/**
 * @brief 復元計画の進捗スロット (staged restore用)
 *
 * サーバ側manifestが送る進捗シグナルを受信し、元の選択全体を基準に単調な進捗として再送出する
 * サーバのtotal値は契約上同じ値だが、UIへは計画開始時に固定した総数を必ず通知する
 *
 * 実行中の計画以外のマニフェストIDを持つシグナルは無視される
 *
 * @param manifestId 進捗を報告してきた計画のマニフェストID
 * @param current 処理済みエントリ数 (manifest全体基準)
 * @param total マニフェストの総エントリ数
 * @param filePath 現在処理中のファイルのベース名
 */
void FileChangeModel::onRestorePlanProgress(const QString &manifestId, int current, int total, const QString &filePath)
{
    // 実行中の計画以外のシグナルは無視する
    if (!m_planActive || m_planManifestId != manifestId) {
        return;
    }

    Q_UNUSED(total);

    const int boundedCurrent = qBound(0, current, m_planTotalFiles);
    m_planLastProgress = qMax(m_planLastProgress, boundedCurrent);
    emit restoreProgress(m_planLastProgress, m_planTotalFiles, filePath);
}

/**
 * @brief 復元計画の完了スロット (staged restore用)
 *
 * サーバ側manifestが送る終端シグナルを受信し、シグナル受信を解除してから復元完了を通知する
 * terminalStateが"completed"の場合のみ成功扱いになる
 *
 * 実行中の計画以外のマニフェストIDを持つシグナルは無視される
 *
 * @param manifestId 完了した計画のマニフェストID
 * @param terminalState 終端状態 ("completed" / "failed" / "cancelled")
 * @param message サーバ側からの追加メッセージ
 */
void FileChangeModel::onRestorePlanFinished(const QString &manifestId, const QString &terminalState, const QString &message)
{
    // 実行中の計画以外のシグナルは無視する
    if (!m_planActive || m_planManifestId != manifestId) {
        return;
    }

    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    const bool completed = (terminalState == QStringLiteral("completed"));
    if (!completed) {
        qWarning() << "Restore plan finished with state:" << terminalState << message;
    }

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit restoreCompleted(completed);
}

/**
 * @brief 復元サービスの消失を失敗として処理する
 *
 * サービス消失後のcancelPlan呼び出しはD-Bus活性化で不要なサービス再起動を招くため、
 * 登録解除以外のtransport呼び出しは行わない。
 */
void FileChangeModel::onRestorePlanServiceVanished()
{
    if (!m_planActive) {
        return;
    }

    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    qWarning() << "Restore service stopped unexpectedly during an active restore plan";

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit errorOccurred(tr("The restore service stopped unexpectedly"));
    emit restoreCompleted(false);
}

/**
 * @brief 設定名を設定
 *
 * Snapperの設定名を設定し、変更された場合はシグナルを発行する
 *
 * @param name 設定名
 */
void FileChangeModel::setConfigName(const QString &name)
{
    if (m_configName != name) {
        m_configName = name;
        emit configNameChanged();
    }
}

/**
 * @brief スナップショット番号を設定
 *
 * 復元元となるスナップショット番号を設定し、変更された場合はシグナルを発行する
 *
 * @param number スナップショット番号
 */
void FileChangeModel::setSnapshotNumber(int number)
{
    if (m_snapshotNumber != number) {
        m_snapshotNumber = number;
        emit snapshotNumberChanged();
    }
    // snapshotNumber を明示セットした場合は「対カレント比較」モードに戻す
    m_betweenMode = false;
    m_flatMode    = false;
}

/**
 * @brief ファイル変更リストを読み込み (非同期) 
 *
 * D-Bus経由でSnapperからファイル変更リストを非同期で取得し、モデルを構築する
 * 読み込み中はloadingプロパティがtrueになる
 */
void FileChangeModel::loadChanges()
{
    // loadChanges()は、対カレント比較を強制 (QMLから呼ばれる想定)
    // loadChangesBetween()は、別ルートで既にm_betweenMode = trueをセット済み
    // そちらからはこの関数を通過しないため、ここではm_betweenModeを偽に戻してよい
    // ただし、loadChangesBetween()側と共通化するために、呼び出し元が明示的にm_betweenModeを制御できるよう、再設定はしない
    if (!m_betweenMode) {
        // モード未設定の場合のみ「対カレント」にする
    }

    if (m_configName.isEmpty() ||
        (!m_betweenMode && m_snapshotNumber <= 0) ||
        (m_betweenMode && (m_compareNumber1 <= 0 || m_compareNumber2 <= 0))) {
        qWarning() << "Invalid config name or snapshot number:" << m_configName << m_snapshotNumber;
        emit errorOccurred("Invalid config name or snapshot number");
        return;
    }

    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnectDbus()) {
            emit errorOccurred("D-Bus connection failed");
            return;
        }
    }

    // ローディング状態ON
    m_loading = true;
    emit loadingChanged();

    const auto requestTimer = QSharedPointer<QElapsedTimer>::create();
    requestTimer->start();

    // 比較モードに応じて D-Bus メソッドを選択
    QDBusPendingCall pendingCall = m_betweenMode
        ? m_dbusInterface->asyncCall("GetFileChangesBetween", m_configName, m_compareNumber1, m_compareNumber2)
        : m_dbusInterface->asyncCall("GetFileChanges", m_configName, m_snapshotNumber);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, requestTimer](QDBusPendingCallWatcher *w) {
        w->deleteLater();

        QDBusPendingReply<QString> reply = *w;

        if (reply.isError()) {
            qWarning() << "Failed to get file changes via D-Bus:" << reply.error().message();
            m_loading = false;
            emit loadingChanged();
            emit errorOccurred(QString("Failed to get file changes: %1").arg(reply.error().message()));
            return;
        }

        const qint64 dbusWaitMs = requestTimer->elapsed();

        QString output = reply.value();
        if (output.isEmpty()) {
            qWarning() << "snapper status command returned empty output";
            m_hasChanges = false;
            emit hasChangesChanged();
            m_loading = false;
            emit loadingChanged();
            emit errorOccurred("No file changes found");
            return;
        }

        m_hasChanges = true;
        emit hasChangesChanged();

        setupModelData(output, m_flatMode);
        qInfo() << "FileChangeModel timing: dbusWait=" << dbusWaitMs << "ms";

        // ローディング状態OFF
        m_loading = false;
        emit loadingChanged();
    });
}

/**
 * @brief 任意の2つのスナップショット間のファイル変更を読み込む
 *
 * num1 --> num2 の差分を取得し、ツリー構造を構築する
 * 復元操作では使用されず、表示 / diff取得専用
 */
void FileChangeModel::loadChangesBetween(int number1, int number2, bool flat)
{
    if (m_configName.isEmpty() || number1 <= 0 || number2 <= 0) {
        emit errorOccurred("Invalid config name or snapshot numbers");
        return;
    }

    m_betweenMode     = true;
    m_flatMode        = flat;  // true = フラット (比較ダイアログ), false = ツリー (復元プレビュー)
    m_compareNumber1  = number1;
    m_compareNumber2  = number2;

    // 比較先スナップショットを主としておく (RestoreFiles() 呼び出し時の参照用)
    m_snapshotNumber  = number2;
    loadChanges();
}

/**
 * @brief 「対カレント」比較モードを強制して ロードする補助は現状未使用
 *
 * QML側でsetSnapshotNumber --> loadChanges()の順で呼び出す場合、
 * m_betweenModeが残らないよう、setSnapshotNumberでリセットする
 */
void FileChangeModel::restoreSingleFile(const QString &filePath)
{
    if (m_configName.isEmpty() || m_snapshotNumber <= 0 || filePath.isEmpty()) {
        emit errorOccurred(tr("Invalid parameters for restore"));
        return;
    }

    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnectDbus()) {
            emit errorOccurred(tr("D-Bus connection failed"));
            return;
        }
    }

    m_cancelRequested = false;
    m_restoreHasError = false;
    m_totalFilesCount = 1;
    m_processedFilesCount = 0;

    // 事前認証 (Authenticate D-Busメソッド) は撤廃 (P0-2)
    // Polkit認証は RestoreFiles / RestoreFilesDirect 呼び出し時に都度行われ、
    // auth_admin_keepにより短時間の連続操作ではUX的にも1回プロンプトと同等になる

    QStringList filePaths;
    filePaths << filePath;

    // ツリーからchangeTypeを取得
    QString changeType = QStringLiteral("modified");
    QModelIndex idx = findItemIndex(m_rootItem, filePath);
    if (idx.isValid()) {
        FileChangeItem *item = getItem(idx);
        if (item) {
            changeType = changeTypeToString(item->changeType());
        }
    }
    QStringList changeTypes;
    changeTypes << changeType;

    // 復元方式に応じたD-Busメソッドを呼び出し
    QString methodName = m_useDirectRestore ? "RestoreFilesDirect" : "RestoreFiles";
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
        m_dbusInterface->asyncCall(methodName, m_configName, m_snapshotNumber, filePaths, changeTypes), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        QDBusPendingReply<bool> reply = *watcher;
        watcher->deleteLater();

        if (reply.isError()) {
            qWarning() << "Single file restore failed:" << reply.error().message();
            emit errorOccurred(tr("Restore failed: %1").arg(reply.error().message()));
            emit restoreCompleted(false);
        }
        else {
            emit restoreCompleted(reply.value());
        }
    });
}

/**
 * @brief ファイルの差分と詳細情報を非同期で一括取得
 *
 * D-Bus経由でGetFileDiffAndDetailsを非同期で呼び出し、結果をfileDiffAndDetailsReadyシグナルで通知する
 *
 * @param filePath 対象ファイルのパス
 */
void FileChangeModel::getFileDiffAndDetails(const QString &filePath)
{
    if (m_configName.isEmpty() || filePath.isEmpty()) {
        return;
    }

    if (!m_betweenMode && m_snapshotNumber <= 0) {
        return;
    }

    if (m_betweenMode && (m_compareNumber1 <= 0 || m_compareNumber2 <= 0)) {
        return;
    }

    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnectDbus()) {
            emit errorOccurred("D-Bus connection failed");
            return;
        }
    }

    // Pre↔Post表示モード (m_betweenMode = true) では2スナップショット間のdiffを取得し、
    // それ以外 (対カレント) では従来どおり GetFileDiffAndDetails() を呼ぶ
    // どちらも戻り値フォーマット (details + ---DIFF_SEPARATOR--- + diff) は同一
    QDBusPendingCall pendingCall = m_betweenMode
        ? m_dbusInterface->asyncCall("GetFileDiffBetween", m_configName, m_compareNumber1, m_compareNumber2, filePath)
        : m_dbusInterface->asyncCall("GetFileDiffAndDetails", m_configName, m_snapshotNumber, filePath);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, filePath](QDBusPendingCallWatcher *w) {
        w->deleteLater();

        QDBusPendingReply<QString> reply = *w;

        if (reply.isError()) {
            qWarning() << "Failed to get file diff and details:" << reply.error().message();
            emit fileDiffAndDetailsReady(filePath, QVariantMap(), QString());
            return;
        }

        QString result = reply.value();
        if (result.isEmpty()) {
            emit fileDiffAndDetailsReady(filePath, QVariantMap(), QString());
            return;
        }

        // セパレータでdetails部とdiff部を分割
        const QString separator = "---DIFF_SEPARATOR---\n";
        int sepIndex = result.indexOf(separator);

        QString detailsPart;
        QString diffPart;
        if (sepIndex >= 0) {
            detailsPart = result.left(sepIndex);
            diffPart = result.mid(sepIndex + separator.length());
        }
        else {
            detailsPart = result;
        }

        // details部をQVariantMapにパース
        QVariantMap details;
        const QStringList lines = detailsPart.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            int eqPos = line.indexOf('=');
            if (eqPos > 0) {
                details[line.left(eqPos)] = line.mid(eqPos + 1);
            }
        }

        emit fileDiffAndDetailsReady(filePath, details, diffPart);
    });
}

/**
 * @brief 指定された位置のインデックスを取得
 *
 * モデル内の指定された行、列、親インデックスに対応するQModelIndexを返す
 *
 * @param row 行番号
 * @param column 列番号
 * @param parent 親のQModelIndex
 * @return QModelIndex
 */
QModelIndex FileChangeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    FileChangeItem *parentItem = getItem(parent);
    FileChangeItem *childItem = parentItem->child(row);

    if (childItem) {
        return createIndex(row, column, childItem);
    }

    return QModelIndex();
}

/**
 * @brief 親のインデックスを取得
 *
 * 指定された子アイテムの親のQModelIndexを返す
 *
 * @param child 子のQModelIndex
 * @return 親のQModelIndex
 */
QModelIndex FileChangeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return QModelIndex();
    }

    FileChangeItem *childItem = getItem(child);
    FileChangeItem *parentItem = childItem->parent();

    if (parentItem == m_rootItem || parentItem == nullptr) {
        return QModelIndex();
    }

    return createIndex(parentItem->row(), 0, parentItem);
}

/**
 * @brief 行数を取得
 *
 * 指定された親アイテムの持つ子アイテムの数を返す
 *
 * @param parent 親のQModelIndex
 * @return 行数 (子アイテムの数)
 */
int FileChangeModel::rowCount(const QModelIndex &parent) const
{
    FileChangeItem *parentItem = getItem(parent);
    return parentItem->childCount();
}

/**
 * @brief 列数を取得
 *
 * このモデルは常に1列である
 *
 * @param parent 親のQModelIndex (未使用)
 * @return 列数 (常に1)
 */
int FileChangeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

/**
 * @brief データを取得
 *
 * 指定されたインデックスとロールに対応するデータを返す
 *
 * @param index データを取得したいQModelIndex
 * @param role データのロール (PathRole, NameRoleなど)
 * @return データのQVariant
 */
QVariant FileChangeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    FileChangeItem *item = getItem(index);

    switch (role) {
    case PathRole:
        return item->path();
    case NameRole:
        return item->name();
    case ChangeTypeRole:
        return item->changeType();
    case IsDirectoryRole:
        return item->isDirectory();
    case IsCheckedRole:
        return item->isChecked();
    case StatusFlagsRole:
        return item->statusFlags();
    case Qt::DisplayRole:
        return item->name();
    default:
        return QVariant();
    }
}

/**
 * @brief ロール名を取得
 *
 * QML等で使用するロール名のマッピングを返す
 *
 * @return ロール名のハッシュマップ
 */
QHash<int, QByteArray> FileChangeModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[PathRole] = "filePath";
    roles[NameRole] = "fileName";
    roles[ChangeTypeRole] = "changeType";
    roles[IsDirectoryRole] = "isDirectory";
    roles[IsCheckedRole] = "isChecked";
    roles[StatusFlagsRole] = "statusFlags";
    return roles;
}

/**
 * @brief モデルデータの構築
 *
 * ファイル変更リストからツリー構造のモデルデータを構築する
 * 重複を除外し、ディレクトリ階層を適切に生成
 *
 * @param changeOutput Snapperから返された改行区切りの変更出力
 * @param flatMode trueの場合はフラット表示用に構築する
 */
void FileChangeModel::setupModelData(const QString &changeOutput, bool flatMode)
{
    QElapsedTimer parsePreparationTimer;
    parsePreparationTimer.start();

    QVector<ChangeInfo> changes;
    QSet<QString> seenPaths;
    QSet<QString> parentPaths;
    const QStringList lines = changeOutput.split('\n', Qt::SkipEmptyParts);
    changes.reserve(lines.size());

    for (const QString &line : lines) {
        ChangeInfo info;
        if (!parseChangeRecord(line, &info) || seenPaths.contains(info.path)) {
            continue;
        }

        info.type = parseChangeType(info.statusFlags.at(0));
        seenPaths.insert(info.path);
        collectParentPaths(info.path, &parentPaths);
        changes.append(info);
    }

    QVector<ChangeInfo> treeChanges = changes;
    if (!flatMode) {
        std::sort(treeChanges.begin(), treeChanges.end(), [](const ChangeInfo &left, const ChangeInfo &right) {
            return left.path < right.path;
        });
    }
    const qint64 parsePreparationMs = parsePreparationTimer.elapsed();

    QElapsedTimer detachedTreeConstructionTimer;
    detachedTreeConstructionTimer.start();
    FileChangeItem *newRootItem = new FileChangeItem("", FileChangeItem::Modified);

    // --- フラットモード: 2 スナップショット比較ダイアログ用 ---
    // ListViewは、QAbstractItemModelのルート直下しか反復しないため、各変更をm_rootItemの直接の子として1行ずつ追加する
    // 中間ディレクトリは生成せず、重複除去のみ行う
    if (flatMode) {
        for (const ChangeInfo &info : changes) {
            const QString itemPath = info.isDirectory ? info.path + "/" : info.path;
            FileChangeItem *item = new FileChangeItem(itemPath, info.type, info.statusFlags, newRootItem);
            newRootItem->appendChild(item);
        }
    }
    else {
        // --- ツリーモード (従来): 対カレント比較 / 復元UI用 ---
        // アイテムマップ：正規化パス (スラッシュなし) --> FileChangeItem
        QHash<QString, FileChangeItem*> itemMap;
        itemMap[""] = newRootItem;

        // 変更リストをパス順に処理してツリーを構築する。親パスは解析時に収集済みであり、
        // 全パスの接頭辞を総当たりする必要はない。
        for (const ChangeInfo &info : treeChanges) {
            QStringList pathParts = info.path.split('/', Qt::SkipEmptyParts);
            QString currentPath = "";
            FileChangeItem *parentItem = newRootItem;

            for (int i = 0; i < pathParts.size(); ++i) {
                QString part = pathParts[i];
                currentPath += "/" + part;

                bool isLastPart = (i == pathParts.size() - 1);

                if (isLastPart) {
                    // 最終パート：変更があったファイルまたはディレクトリ
                    if (!itemMap.contains(currentPath)) {
                        const bool isDirectory = info.isDirectory || parentPaths.contains(info.path);
                        QString itemPath = isDirectory ? (currentPath + "/") : currentPath;
                        FileChangeItem *item = new FileChangeItem(itemPath, info.type, info.statusFlags, parentItem);
                        parentItem->appendChild(item);
                        itemMap[currentPath] = item;
                    }
                }
                else {
                    // 中間ディレクトリの処理
                    if (!itemMap.contains(currentPath)) {
                        FileChangeItem *dirItem = new FileChangeItem(currentPath + "/", FileChangeItem::Modified, QString(), parentItem);
                        parentItem->appendChild(dirItem);
                        itemMap[currentPath] = dirItem;
                    }
                    parentItem = itemMap[currentPath];
                }
            }
        }
    }

    const qint64 detachedTreeConstructionMs = detachedTreeConstructionTimer.elapsed();
    QElapsedTimer modelPublicationTimer;
    modelPublicationTimer.start();
    FileChangeItem *oldRootItem = m_rootItem;
    beginResetModel();
    m_rootItem = newRootItem;
    endResetModel();
    const qint64 modelPublicationMs = modelPublicationTimer.elapsed();
    delete oldRootItem;

    qInfo() << "FileChangeModel timing: responseParsePreparation=" << parsePreparationMs
            << "ms detachedTreeConstruction=" << detachedTreeConstructionMs
            << "ms modelResetPublication=" << modelPublicationMs
            << "ms records=" << changes.size();
}

/**
 * @brief モデルをクリア
 *
 * ルートアイテムを削除して新しいルートアイテムを作成し、モデルをリセットする
 */
void FileChangeModel::clearModel()
{
    delete m_rootItem;
    m_rootItem = new FileChangeItem("", FileChangeItem::Modified);
}

/**
 * @brief インデックスからアイテムを取得
 *
 * QModelIndexに対応するFileChangeItemを返す
 *
 * @param index QModelIndex
 * @return FileChangeItemへのポインタ (無効なインデックスの場合はルートアイテム)
 */
FileChangeItem *FileChangeModel::getItem(const QModelIndex &index) const
{
    if (index.isValid()) {
        FileChangeItem *item = static_cast<FileChangeItem*>(index.internalPointer());
        if (item) {
            return item;
        }
    }
    return m_rootItem;
}

/**
 * @brief 変更タイプをパース
 *
 * Snapperのステータス文字から変更タイプを判定する
 *
 * @param statusChar ステータス文字 ('+', '-', 'c', 'm', 't'など)
 * @return 変更タイプ
 */
FileChangeItem::ChangeType FileChangeModel::parseChangeType(const QChar &statusChar)
{
    switch (statusChar.toLatin1()) {
    case '+':
        return FileChangeItem::Created;
    case '-':
        return FileChangeItem::Deleted;
    case 'c':
    case 'm':
        return FileChangeItem::Modified;
    case 't':
        return FileChangeItem::TypeChanged;
    default:
        return FileChangeItem::Modified;
    }
}

/**
 * @brief アイテムのチェック状態を設定
 *
 * 指定されたパスのアイテムのチェック状態を設定する
 * ディレクトリの場合は配下の全てのアイテムも再帰的に設定される
 *
 * チェックを外す場合は、明示的にチェックを外したフラグが立てられる
 *
 * @param filePath ファイルパス
 * @param checked チェック状態 (true/false)
 */
void FileChangeModel::setItemChecked(const QString &filePath, bool checked)
{
    // ルートアイテムから指定されたパスのアイテムを検索
    QModelIndex index = findItemIndex(m_rootItem, filePath);
    if (index.isValid()) {
        FileChangeItem *item = getItem(index);

        // チェックを外す場合は、明示的にチェックを外したフラグを立てる
        if (!checked) {
            item->setExplicitlyUnchecked(true);
        }
        else {
            // チェックを入れる場合は、フラグをクリア
            item->setExplicitlyUnchecked(false);
        }

        setItemCheckedRecursive(item, index, checked);
    }
}

/**
 * @brief アイテムのチェック状態を再帰的に設定
 *
 * 指定されたアイテムとその配下の全てのアイテムのチェック状態を再帰的に設定する
 * 明示的にチェックを外された子アイテムはスキップされる
 *
 * @param item 対象のFileChangeItem
 * @param index 対象のQModelIndex
 * @param checked チェック状態 (true/false)
 */
void FileChangeModel::setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked)
{
    if (!item) {
        return;
    }

    // 現在のアイテムのチェック状態を設定
    item->setChecked(checked);
    emit dataChanged(index, index, {IsCheckedRole});

    // ディレクトリの場合、子要素を再帰的にチェック/アンチェック
    if (item->isDirectory()) {
        for (int i = 0; i < item->childCount(); ++i) {
            FileChangeItem *child = item->child(i);

            // チェックを入れる場合、明示的にチェックを外された子アイテムはスキップ
            if (checked && child->isExplicitlyUnchecked()) {
                continue;
            }

            QModelIndex childIndex = this->index(i, 0, index);
            setItemCheckedRecursive(child, childIndex, checked);
        }
    }
}

/**
 * @brief チェックされたアイテムのリストを取得
 *
 * チェックされた全てのアイテムのパスを収集し、復元順序に最適化してリストを返す
 * ディレクトリ階層の深い順にソートされる
 *
 * @return チェックされたアイテムのパスリスト
 */
QStringList FileChangeModel::getCheckedItems() const
{
    QStringList checkedPaths;
    collectCheckedItems(m_rootItem, checkedPaths);

    // 重複を除外
    QSet<QString> uniquePaths(checkedPaths.begin(), checkedPaths.end());
    checkedPaths = QStringList(uniquePaths.begin(), uniquePaths.end());

    // ディレクトリかファイルかを判定し、分類
    QStringList directories;
    QStringList files;

    for (const QString &path : checkedPaths) {
        // パスが他のパスの親である場合はディレクトリ
        bool isDirectory = false;
        for (const QString &otherPath : checkedPaths) {
            if (otherPath != path && otherPath.startsWith(path + "/")) {
                isDirectory = true;
                break;
            }
        }

        if (isDirectory) {
            directories.append(path);
        }
        else {
            files.append(path);
        }
    }

    // 復元順序を最適化：深い階層から浅い階層へソート
    // 深さでソート (スラッシュの数が多い方が深い)
    auto sortByDepth = [](const QString &a, const QString &b) {
        int depthA = a.count('/');
        int depthB = b.count('/');
        if (depthA != depthB) {
            return depthA > depthB; // 深い方が先
        }
        return a > b; // 同じ深さなら辞書順の逆順
    };

    std::sort(directories.begin(), directories.end(), sortByDepth);
    std::sort(files.begin(), files.end(), sortByDepth);

    // 復元リストを構築：ファイル --> ディレクトリの順
    // (深い階層から浅い階層へ)
    QStringList sortedPaths;
    sortedPaths.append(files);
    sortedPaths.append(directories);

    return sortedPaths;
}

/**
 * @brief 復元バッチサイズを更新する
 *
 * 入力値を 1..1000 に丸めた上で保持し、設定ファイルへ永続化する
 *
 * @param size ユーザが要求したバッチサイズ
 */
void FileChangeModel::setRestoreBatchSize(int size)
{
    // UIや設定値の揺れを吸収するため、有効範囲へ丸める
    size = qBound(1, size, 1000);
    if (m_restoreBatchSize != size) {
        m_restoreBatchSize = size;
        QSettings settings("Presire", "qSnapper");
        settings.setValue("restore/batchSize", m_restoreBatchSize);
        emit restoreBatchSizeChanged();
    }
}

/**
 * @brief 直接復元モードの有効 / 無効を更新する
 *
 * 値が変化した場合のみ内部状態と設定ファイルを更新する
 *
 * @param use trueなら直接復元を使用する
 */
void FileChangeModel::setUseDirectRestore(bool use)
{
    if (m_useDirectRestore != use) {
        m_useDirectRestore = use;
        QSettings settings("Presire", "qSnapper");
        settings.setValue("restore/useDirectMethod", m_useDirectRestore);
        emit useDirectRestoreChanged();
    }
}

/**
 * @brief ChangeType列挙値をD-Bus送信用の文字列へ変換する
 *
 * RestoreFiles系APIが期待するlower-case文字列へ正規化する
 *
 * @param type 変換対象の変更種別
 * @return 対応するchangeType文字列
 */
QString FileChangeModel::changeTypeToString(FileChangeItem::ChangeType type)
{
    switch (type) {
    case FileChangeItem::Created:     return QStringLiteral("created");
    case FileChangeItem::Deleted:     return QStringLiteral("deleted");
    case FileChangeItem::Modified:    return QStringLiteral("modified");
    case FileChangeItem::TypeChanged: return QStringLiteral("typechanged");
    }
    return QStringLiteral("modified");
}

/**
 * @brief 復元計画に送出するパスを正規化する
 *
 * 末尾のスラッシュを除去し、連続するスラッシュを単一にまとめる
 * サーバ側はこれらを許容するが、正規化して送ることでmanifestの内容を安定させる
 *
 * @param path 正規化対象のパス
 * @return 正規化済みのパス
 */
QString FileChangeModel::normalizeRestorePlanPath(const QString &path)
{
    QString normalized = path;
    while (normalized.size() > 1 && normalized.endsWith('/')) {
        normalized.chop(1);
    }
    static const QRegularExpression repeatedSlashes(QStringLiteral("/{2,}"));
    normalized.replace(repeatedSlashes, QStringLiteral("/"));
    return normalized;
}

/**
 * @brief 復元計画に送出できるエントリかどうかを判定する
 *
 * サーバ側 (StageRestoreEntries) は1件でも不正なエントリがあるとチャンク全体を拒否するため、
 * クライアント側で同じ規則を先に検査して不正エントリを除外する (レガシーのRestoreFiles*が不正エントリを黙ってスキップしていた挙動に合わせる)
 *
 * @param path 正規化済みのパス
 * @param changeType 変更タイプ文字列
 * @return 送出可能な場合はtrue
 */
bool FileChangeModel::isValidRestorePlanEntry(const QString &path, const QString &changeType)
{
    if (!path.startsWith('/')) {
        return false;
    }
    if (path == QStringLiteral("/.snapshots") || path.startsWith(QStringLiteral("/.snapshots/"))) {
        return false;
    }
    if (changeType != QStringLiteral("created") && changeType != QStringLiteral("deleted")
            && changeType != QStringLiteral("modified") && changeType != QStringLiteral("typechanged")) {
        return false;
    }

    // '.' / '..' 成分を含まないこと
    const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QLatin1String(".") || component == QLatin1String("..")) {
            return false;
        }
    }

    // 制御文字 (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含まないこと
    for (const QChar &ch : path) {
        const ushort ucs = ch.unicode();
        if (ucs <= 0x1F || ucs == 0x7F || (ucs >= 0x80 && ucs <= 0x9F)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief チェック済みアイテムをchangeType付きで再帰収集する
 *
 * ディレクトリは自身の変更と配下の変更を分けて扱い、
 * 最終的にRestoreFiles系APIへ渡せるpath / changeType配列を構築する
 *
 * @param parent 走査開始ノード
 * @param paths 収集したパスの格納先
 * @param changeTypes 収集したchangeType文字列の格納先
 */
void FileChangeModel::collectCheckedItemsWithTypes(FileChangeItem *parent, QStringList &paths, QStringList &changeTypes) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        if (child->isChecked()) {
            // チェックされたノードは、自身と必要なら配下の両方を収集対象にする
            QString itemPath = child->path();

            // パスを正規化 (末尾のスラッシュを削除)
            if (itemPath.endsWith('/') && itemPath.length() > 1) {
                itemPath = itemPath.left(itemPath.length() - 1);
            }

            bool hasChildren = (child->childCount() > 0);
            // Modified は「親ディレクトリとして存在するだけ」の場合があるため、
            // ディレクトリエントリ自体を送るかどうかは別途判定する
            bool isActualChange = (child->changeType() != FileChangeItem::Modified);

            if (hasChildren) {
                // 子要素があるアイテム = ディレクトリ
                if (isActualChange && !itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                    changeTypes.append(changeTypeToString(child->changeType()));
                }
                // 配下を再帰的に収集 (collectAllFilesRecursiveと同等の処理)
                collectCheckedItemsWithTypes(child, paths, changeTypes);
            }
            else {
                if (!itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                    changeTypes.append(changeTypeToString(child->changeType()));
                }
            }
        }
        else {
            // 親が未チェックでも、配下に個別選択された項目があれば拾う
            collectCheckedItemsWithTypes(child, paths, changeTypes);
        }
    }
}

/**
 * @brief チェックされたアイテムを復元
 *
 * チェックされた全てのアイテムを復元する (staged restore)
 * 事前にエントリを上限付きチャンクへ分割して認証前にstagingし、全チャンクのstaging完了後にcommitを1度だけ呼び出す
 * Polkitプロンプトはcommit時に1度だけ表示され、以降の進捗/完了はサーバ側manifestのシグナルで駆動される
 *
 * @return 復元処理が開始された場合: true、エラーの場合: false
 */
bool FileChangeModel::restoreCheckedItems()
{
    // 両方のモードでchangeTypeを収集する (StageRestoreEntriesでchangeTypes必須)
    QStringList checkedPaths;
    QStringList checkedChangeTypes;

    collectCheckedItemsWithTypes(m_rootItem, checkedPaths, checkedChangeTypes);

    // 送信前にクライアント側で検証・正規化し、不正エントリを除外する
    // 正規化後のパスで重複を除き、最初の出現順を維持する
    QStringList planPaths;
    QStringList planChangeTypes;
    QSet<QString> seenPaths;
    const int entryCount = qMin(checkedPaths.size(), checkedChangeTypes.size());
    for (int i = 0; i < entryCount; ++i) {
        const QString normalized = normalizeRestorePlanPath(checkedPaths.at(i));
        if (!isValidRestorePlanEntry(normalized, checkedChangeTypes.at(i))) {
            qWarning() << "Skipping invalid restore entry:" << checkedPaths.at(i);
            continue;
        }
        if (seenPaths.contains(normalized)) {
            continue;
        }
        seenPaths.insert(normalized);
        planPaths.append(normalized);
        planChangeTypes.append(checkedChangeTypes.at(i));
    }

    if (planPaths.isEmpty()) {
        emit errorOccurred(tr("No files selected for restoration"));
        emit restoreCompleted(false);
        return false;
    }

    if (m_configName.isEmpty() || m_snapshotNumber <= 0) {
        emit errorOccurred("Invalid config name or snapshot number");
        emit restoreCompleted(false);
        return false;
    }

    if (!m_restorePlanTransport) {
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }

    // System Bus transportは、従来の復元経路と同じくサービス再接続を試みる
    // テストtransportはD-Bus接続なしで実行できるよう、この確認を適用しない
    if (m_restorePlanTransport == m_defaultRestorePlanTransport
            && (!m_dbusInterface || !m_dbusInterface->isValid())
            && !reconnectDbus()) {
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }

    // 復元計画の状態を初期化する (前回実行の残留状態を消す)
    m_planManifestId.clear();
    m_planPaths = planPaths;
    m_planChangeTypes = planChangeTypes;
    m_planNextStageIndex = 0;
    m_planCommitted = false;
    m_planActive = true;
    m_planSignalsSubscribed = false;
    m_planCancelRequested = false;
    m_planTotalFiles = planPaths.size();
    m_planLastProgress = 0;
    m_cancelRequested = false;
    m_totalFilesCount = planPaths.size();
    m_processedFilesCount = 0;

    // 進捗/完了はサーバ側manifestのシグナルで駆動する
    if (!m_restorePlanTransport->subscribePlanSignals(this)) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        resetRestorePlanState();
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }
    m_planSignalsSubscribed = true;

    // 認証なしでstaging計画を開始する (Polkitプロンプトはcommit時に1度だけ出る)
    const QString restoreMode = m_useDirectRestore ? QStringLiteral("direct") : QStringLiteral("yast");
    m_restorePlanTransport->beginPlan(m_configName, m_snapshotNumber, restoreMode,
                                      [this](bool ok, const QString &manifestId, const QString &error) {
                                          onPlanBeginFinished(ok, manifestId, error);
                                      });

    return true;
}

/**
 * @brief staging計画の作成結果を処理する
 *
 * beginPlanの結果を受け、成功した場合は先頭チャンクのstagingを開始する
 *
 * @param ok 計画作成に成功した場合: true
 * @param manifestId 作成された計画のマニフェストID
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanBeginFinished(bool ok, const QString &manifestId, const QString &error)
{
    // beginの応答がキャンセル後に届いた場合も、発行済みmanifestをベストエフォートで破棄する
    if (!m_planActive) {
        if (m_cancelRequested && !manifestId.isEmpty()) {
            m_restorePlanTransport->cancelPlan(manifestId,
                                               [](bool ok, const QString &cancelError) {
                                                   Q_UNUSED(ok);
                                                   Q_UNUSED(cancelError);
                                               });
        }
        return;
    }

    if (m_cancelRequested) {
        if (!manifestId.isEmpty()) {
            m_planManifestId = manifestId;
            m_planCancelRequested = true;
            m_planActive = false;
            if (m_planSignalsSubscribed) {
                m_restorePlanTransport->unsubscribePlanSignals(this);
                m_planSignalsSubscribed = false;
            }
            m_restorePlanTransport->cancelPlan(m_planManifestId,
                                               [](bool ok, const QString &cancelError) {
                                                   Q_UNUSED(ok);
                                                   Q_UNUSED(cancelError);
                                               });
        }
        else {
            m_planActive = false;
            if (m_planSignalsSubscribed) {
                m_restorePlanTransport->unsubscribePlanSignals(this);
                m_planSignalsSubscribed = false;
            }
        }
        resetRestorePlanState();
        emit restoreCompleted(false);
        return;
    }

    if (!ok || manifestId.isEmpty()) {
        if (!manifestId.isEmpty()) {
            m_planManifestId = manifestId;
        }
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to begin restore plan") : error);
        return;
    }

    m_planManifestId = manifestId;
    stageNextPlanChunk();
}

/**
 * @brief 次のチャンクをstagingする
 *
 * チャンクは必ず1件ずつ順次送出する (サーバ側は計画ごとに単一スレッドで処理するため、並列送出は行わない)
 * 全チャンクのstagingが完了したらcommitを1度だけ呼び出して、計画を凍結する
 */
void FileChangeModel::stageNextPlanChunk()
{
    if (!m_planActive || m_cancelRequested || m_planCommitted) {
        return;
    }

    // サーバ側の1チャンク上限に合わせたチャンクサイズ
    // (m_restoreBatchSizeは1..1000に丸め済みのため常に収まるが、念のためクランプする)
    static constexpr int kMaxEntriesPerStageChunk = 5000;  // src/dbusservice/restoremanifest.hのkMaxEntriesPerStageChunkと同期
    const int chunkSize = qBound(1, m_restoreBatchSize, kMaxEntriesPerStageChunk);

    if (m_planManifestId.isEmpty()) {
        finishRestorePlanWithError(tr("Restore plan has no manifest id"));
        return;
    }

    if (m_planNextStageIndex >= m_planPaths.size()) {
        // 全チャンクのstaging完了 -> 計画を凍結して認証と実行を開始する
        commitRestorePlan();
        return;
    }

    const int chunkEnd = qMin(m_planNextStageIndex + chunkSize, m_planPaths.size());
    QStringList chunkPaths;
    QStringList chunkTypes;
    for (int i = m_planNextStageIndex; i < chunkEnd; ++i) {
        chunkPaths.append(m_planPaths.at(i));
        chunkTypes.append(m_planChangeTypes.at(i));
    }

    // チャンクの送出前にインデックスを進める (失敗時はフロー自体を中断するため巻き戻し不要)
    m_planNextStageIndex = chunkEnd;

    m_restorePlanTransport->stageEntries(m_planManifestId, chunkPaths, chunkTypes,
                                         [this](bool ok, const QString &error) {
                                             onPlanStageFinished(ok, error);
                                         });
}

/**
 * @brief チャンクのstaging結果を処理する
 *
 * 成功した場合は次のチャンクをstagingし、失敗した場合はフローを中断する
 *
 * @param ok stagingに成功した場合: true
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanStageFinished(bool ok, const QString &error)
{
    if (!m_planActive || m_cancelRequested || m_planCommitted) {
        return;
    }

    if (!ok) {
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to stage restore entries") : error);
        return;
    }

    stageNextPlanChunk();
}

/**
 * @brief 復元計画をcommit (凍結) する
 *
 * この呼び出しでサーバ側のPolkit認証が1度だけ行われ、認証後は実行がサーバ側で非同期に継続される
 * 以降のUI更新はシグナル駆動になる
 */
void FileChangeModel::commitRestorePlan()
{
    if (!m_planActive || m_cancelRequested || m_planCommitted || m_planManifestId.isEmpty()) {
        return;
    }

    // commit呼び出しを発行した時点で計画は凍結処理中とみなし、その間のcancelRestore()も実行中計画として扱う
    m_planCommitted = true;
    m_restorePlanTransport->commitPlan(m_planManifestId,
                                       [this](bool ok, const QString &error) {
                                           onPlanCommitFinished(ok, error);
                                       });
}

/**
 * @brief 復元計画のcommit結果を処理する
 *
 * commitに成功したらフローはシグナル待ちに移行する。失敗した場合は
 * フローを中断する。
 *
 * @param ok commitに成功した場合: true
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanCommitFinished(bool ok, const QString &error)
{
    if (!m_planActive) {
        return;
    }

    if (!ok) {
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to commit restore plan") : error);
        return;
    }

    // 計画は凍結済み
    // 以降はrestorePlanProgress / restorePlanFinishedで駆動される
}

/**
 * @brief 復元計画をエラー完了させる
 *
 * エラー発生時は以降のtransport呼び出しを行わず (サーバ側に残ったstaging計画の破棄を促すベストエフォートのcancelPlanのみ例外)、
 * シグナル受信を解除してエラーと完了を通知し、状態をリセットする
 *
 * @param message ユーザーに表示するエラーメッセージ
 */
void FileChangeModel::finishRestorePlanWithError(const QString &message)
{
    if (!m_planActive) {
        return;
    }

    const QString manifestId = m_planManifestId;
    RestorePlanTransport *transport = m_restorePlanTransport;
    m_planActive = false;

    // 以降の遅延コールバックやserver signalが完了通知を重ねないよう、transport呼び出しの前に購読を解除する
    if (m_planSignalsSubscribed) {
        transport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    // マニフェストIDが既に発行されている場合は、サーバ側に残った計画をエージング任せにせず破棄させる (ベストエフォート)
    if (!manifestId.isEmpty() && !m_planCancelRequested) {
        m_planCancelRequested = true;
        transport->cancelPlan(manifestId,
                              [](bool ok, const QString &cancelError) {
                                  Q_UNUSED(ok);
                                  Q_UNUSED(cancelError);
                              });
    }

    qWarning() << "Restore plan failed:" << message;

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit errorOccurred(message);
    emit restoreCompleted(false);
}

/**
 * @brief 復元計画の実行状態をリセットする
 *
 * 次回のrestoreCheckedItems()がクリーンな状態から開始できるようにする
 */
void FileChangeModel::resetRestorePlanState()
{
    m_planManifestId.clear();
    m_planPaths.clear();
    m_planChangeTypes.clear();
    m_planNextStageIndex = 0;
    m_planCommitted = false;
    m_planActive = false;
    m_planSignalsSubscribed = false;
    m_planCancelRequested = false;
    m_planTotalFiles = 0;
    m_planLastProgress = 0;
}

/**
 * @brief アイテムのインデックスを検索
 *
 * 指定されたパスのアイテムを再帰的に検索し、そのQModelIndexを返す
 *
 * @param parent 検索開始アイテム
 * @param path 検索するパス
 * @return 見つかったアイテムのQModelIndex (見つからない場合は無効なインデックス)
 */
QModelIndex FileChangeModel::findItemIndex(FileChangeItem *parent, const QString &path) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);
        if (child->path() == path) {
            return createIndex(i, 0, child);
        }

        // 再帰的に子要素を検索
        QModelIndex childIndex = findItemIndex(child, path);
        if (childIndex.isValid()) {
            return childIndex;
        }
    }

    return QModelIndex();
}

/**
 * @brief チェックされたアイテムを収集
 *
 * チェックされたアイテムのパスを再帰的に収集する
 * ディレクトリの場合は、配下の全てのファイルも収集される
 *
 * @param parent 収集開始アイテム
 * @param paths 収集されたパスのリスト (出力)
 */
void FileChangeModel::collectCheckedItems(FileChangeItem *parent, QStringList &paths) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        if (child->isChecked()) {
            QString itemPath = child->path();

            // パスを正規化 (末尾のスラッシュを削除)
            if (itemPath.endsWith('/') && itemPath.length() > 1) {
                itemPath = itemPath.left(itemPath.length() - 1);
            }

            bool hasChildren = (child->childCount() > 0);
            bool isActualChange = (child->changeType() != FileChangeItem::Modified);

            if (hasChildren) {
                // 子要素があるアイテム = ディレクトリ

                // 実際に変更されたディレクトリのみ追加
                if (isActualChange && !itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                }

                // 配下を再帰的に収集
                collectAllFilesRecursive(child, paths);
            }
            else {
                // 子要素がないアイテム
                // パスと変更タイプから判断
                if (!itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                }
            }
        }
        else {
            // チェックされていないアイテムでも、子要素を再帰的に確認
            collectCheckedItems(child, paths);
        }
    }
}

/**
 * @brief 全てのファイルを再帰的に収集
 *
 * 指定されたアイテム配下の全てのファイルとディレクトリを再帰的に収集する
 * チェックが外されているアイテムはスキップされる
 *
 * @param parent 収集開始アイテム
 * @param paths 収集されたパスのリスト (出力)
 */
void FileChangeModel::collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        // チェックが外されているアイテムはスキップ
        if (!child->isChecked()) {
            continue;
        }

        QString itemPath = child->path();

        // パスを正規化 (末尾のスラッシュを削除)
        if (itemPath.endsWith('/') && itemPath.length() > 1) {
            itemPath = itemPath.left(itemPath.length() - 1);
        }

        bool hasChildren = (child->childCount() > 0);
        bool isActualChange = (child->changeType() != FileChangeItem::Modified);

        if (hasChildren) {
            // 子要素があるアイテム = ディレクトリ

            // 実際に変更されたディレクトリのみ追加
            if (isActualChange && !itemPath.isEmpty() && itemPath != "/") {
                paths.append(itemPath);
            }

            // さらに配下を再帰的に処理
            collectAllFilesRecursive(child, paths);
        }
        else {
            // 子要素がないアイテム
            if (!itemPath.isEmpty() && itemPath != "/") {
                paths.append(itemPath);
            }
        }
    }
}

/**
 * @brief 復元処理をキャンセル
 *
 * 復元処理のキャンセルを要求する
 *
 * キャンセルはサーバ側の次のチャンク境界で反映されるため、既に復元されたファイルやディレクトリはそのまま残る
 *
 * 復元計画がcommit済みの場合はcancelPlanを送った後、終端シグナル (restorePlanFinished) を待ってから完了を通知する
 * staging中 (commit前) の場合は以降のチャンク送出を停止し、ベストエフォートで計画を破棄してローカルで完了扱いにする
 * 実行中の計画がない場合は従来どおりフラグの設定のみ行う
 */
void FileChangeModel::cancelRestore()
{
    m_cancelRequested = true;
    qWarning() << "Restore operation cancel requested";

    if (!m_planActive) {
        // 復元計画が実行中でない場合はフラグのみ設定する
        return;
    }

    if (m_planCommitted) {
        // commit済み:
        // サーバ側のチャンク境界でキャンセルが効く
        // 完了はrestorePlanFinishedシグナルで判定するため、ここでは楽観的に完了しない
        if (!m_planCancelRequested && !m_planManifestId.isEmpty()) {
            m_planCancelRequested = true;
            m_restorePlanTransport->cancelPlan(m_planManifestId,
                                               [](bool ok, const QString &error) {
                                                   Q_UNUSED(ok);
                                                   Q_UNUSED(error);
                                                   // キャンセル要求の結果は終端シグナルで扱う
                                               });
        }
        return;
    }

    // staging中 (commit前):
    // 以降のチャンクは送出せず、ベストエフォートで計画を破棄してローカルで完了扱いにする
    const QString manifestId = m_planManifestId;
    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    if (!manifestId.isEmpty() && !m_planCancelRequested) {
        m_planCancelRequested = true;
        m_restorePlanTransport->cancelPlan(manifestId,
                                           [](bool ok, const QString &error) {
                                               Q_UNUSED(ok);
                                               Q_UNUSED(error);
                                               // 破棄結果はUI状態に影響しない (ローカル完了扱いのため)
                                           });
    }
    resetRestorePlanState();
    emit restoreCompleted(false);
}

/**
 * @brief テスト専用に復元計画transportを差し替える
 *
 * 所有権は移らないため、差し込んだtransportは呼び出し側が管理する
 * 既定のSystem Bus実装は破棄せず、ポインタのみ差し替える
 *
 * @param transport 差し込むtransport (nullptrの場合は既定のtransportへ戻す)
 */
void FileChangeModel::setRestorePlanTransportForTesting(RestorePlanTransport *transport)
{
    m_restorePlanTransport = transport ? transport : m_defaultRestorePlanTransport;
}
