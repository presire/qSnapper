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
 * 最初の空白区切りだけを消費するため、パス内の空白はそのまま保持する。
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
    , m_currentBatchIndex(0)
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
 * @brief チェック済みアイテムを changeType 付きで再帰収集する
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
 * チェックされた全てのアイテムを復元する
 * 大量のファイルがある場合はバッチに分割して処理される
 *
 * @return 復元処理が開始された場合: true、エラーの場合: false
 */
bool FileChangeModel::restoreCheckedItems()
{
    // 両方のモードでchangeTypeを収集する (RestoreFiles() / RestoreFilesDirect() 共にchangeTypes必須)
    QStringList checkedPaths;
    QStringList checkedChangeTypes;

    collectCheckedItemsWithTypes(m_rootItem, checkedPaths, checkedChangeTypes);
    // 重複除外 (パスとchangeTypeのペアを保持)
    {
        QSet<QString> seen;
        QStringList uniquePaths;
        QStringList uniqueChangeTypes;
        for (int i = 0; i < checkedPaths.size(); ++i) {
            if (!seen.contains(checkedPaths[i])) {
                seen.insert(checkedPaths[i]);
                uniquePaths.append(checkedPaths[i]);
                uniqueChangeTypes.append(checkedChangeTypes[i]);
            }
        }
        checkedPaths = uniquePaths;
        checkedChangeTypes = uniqueChangeTypes;
    }

    if (checkedPaths.isEmpty()) {
        emit errorOccurred(tr("No files selected for restoration"));
        emit restoreCompleted(false);
        return false;
    }

    if (m_configName.isEmpty() || m_snapshotNumber <= 0) {
        emit errorOccurred("Invalid config name or snapshot number");
        emit restoreCompleted(false);
        return false;
    }

    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnectDbus()) {
            emit errorOccurred("D-Bus connection failed");
            emit restoreCompleted(false);
            return false;
        }
    }

    // 事前認証 (Authenticate D-Busメソッド) は撤廃 (P0-2)
    // バッチの最初の RestoreFiles() / RestoreFilesDirect() 呼び出しで、
    // Polkitがプロンプトを出し、以降はauth_admin_keepのcookieにより抑止される

    // D-Busシグナルを接続して進捗を受信
    bool connected = QDBusConnection::systemBus().connect(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        "restoreProgress",
        this,
        SLOT(onRestoreProgress(int,int,QString))
    );

    if (!connected) {
        qWarning() << "Failed to connect to restoreProgress signal";
    }

    // ファイルリストをバッチに分割して順次処理
    m_restoreBatches.clear();
    m_restoreBatchChangeTypes.clear();
    m_currentBatchIndex = 0;
    m_totalFilesCount = checkedPaths.size();
    m_processedFilesCount = 0;
    m_restoreHasError = false;
    m_cancelRequested = false;

    const int batchSize = m_restoreBatchSize;
    for (int i = 0; i < checkedPaths.size(); i += batchSize) {
        QStringList batchPaths;
        QStringList batchTypes;
        for (int j = i; j < qMin(i + batchSize, checkedPaths.size()); ++j) {
            batchPaths.append(checkedPaths[j]);
            batchTypes.append(checkedChangeTypes[j]);
        }
        m_restoreBatches.append(batchPaths);
        m_restoreBatchChangeTypes.append(batchTypes);
    }

    // 最初のバッチを処理
    processNextBatch();

    return true;
}

/**
 * @brief 次のバッチを処理
 *
 * 復元処理のバッチを順次処理する
 * 全てのバッチが完了すると、完了シグナルを発行する
 *
 * キャンセルが要求された場合は、残りのバッチをスキップする
 */
void FileChangeModel::processNextBatch()
{
    // キャンセルが要求された場合は処理を中断
    if (m_cancelRequested) {
        QDBusConnection::systemBus().disconnect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restoreProgress",
            this,
            SLOT(onRestoreProgress(int,int,QString))
        );

        emit restoreCompleted(false);
        return;
    }

    if (m_currentBatchIndex >= m_restoreBatches.size()) {
        // 全てのバッチが処理完了
        QDBusConnection::systemBus().disconnect(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            "restoreProgress",
            this,
            SLOT(onRestoreProgress(int,int,QString))
        );

        emit restoreCompleted(!m_restoreHasError);
        return;
    }

    QStringList batch = m_restoreBatches[m_currentBatchIndex];

    // 非同期呼び出しでファイルを処理 (タイムアウトなし)
    QDBusMessage msg = QDBusMessage::createMethodCall(
        "com.presire.qsnapper.Operations",
        "/com/presire/qsnapper/Operations",
        "com.presire.qsnapper.Operations",
        m_useDirectRestore ? "RestoreFilesDirect" : "RestoreFiles"
    );
    QStringList batchChangeTypes = m_restoreBatchChangeTypes[m_currentBatchIndex];
    msg << m_configName << m_snapshotNumber << batch << batchChangeTypes;

    QDBusPendingCall pendingCall = QDBusConnection::systemBus().asyncCall(msg, -1);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, batch](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<bool> reply = *w;

        if (reply.isError()) {
            qWarning() << "Failed to restore batch:" << reply.error().message();
            m_restoreHasError = true;
            // エラーが発生しても全てのバッチを処理する
        }
        else {
            bool success = reply.value();
            if (!success) {
                m_restoreHasError = true;
            }
        }

        // バッチ完了後、処理済みファイル数を更新
        m_processedFilesCount += batch.size();

        // バッチ完了時に明示的に進捗を通知
        emit restoreProgress(m_processedFilesCount, m_totalFilesCount,
                             QString("Batch %1/%2 completed").arg(m_currentBatchIndex + 1).arg(m_restoreBatches.size()));

        m_currentBatchIndex++;

        w->deleteLater();

        // 次のバッチを処理
        processNextBatch();
    });
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
 * 現在実行中のバッチは完了するが、次のバッチ以降はスキップされる
 * 既に復元されたファイルやディレクトリはそのまま残る
 */
void FileChangeModel::cancelRestore()
{
    m_cancelRequested = true;
    qWarning() << "Restore operation cancel requested";
}
