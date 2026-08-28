/**
 * @file restoreplan_test_service.cpp
 * @brief 隔離root D-Busバス上でstaged restoreのowner境界を検証するためのテスト専用サービス
 *
 * SnapshotOperations (src/dbusservice/snapshotoperations.cpp) の
 * BeginRestorePlan / StageRestoreEntries / CommitRestorePlan /
 * ContinueRestorePlan / GetRestorePlanStatus / CancelRestorePlan の
 * D-Bus adapterロジックを、順序とセマンティクスをVERBATIMに再現したまま
 * 実security core (RestoreManifestRegistry / RestorePlanExecutor /
 * qsnapper::security::*) に接続する。
 *
 * user namespace内では実行できない2箇所のみを意図的に代替する
 * (各代替箇所はDoxygenコメントで明示する):
 *   - CommitRestorePlanのPolkit認証 (checkAuthorization) は
 *     authorizeForTest() へ置き換える
 *   - RestorePlanExecutorのEntryApplierはlibsnapperでmountした実ファイルを
 *     コピーする代わりにapplyRestoreEntryForTest() へ置き換える
 *
 * libsnapper / PolkitQt1 / btrfsutil にはリンクしない。
 */

#include "filesystemhelpers.h"
#include "inputvalidator.h"
#include "restoremanifest.h"
#include "restoreplanexecutor.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QMap>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <csignal>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace {

/// SIGTERM受信を伝えるためのself-pipe (書き込み端をシグナルハンドラから使用)
int g_sigtermFd[2] = {-1, -1};

/**
 * @brief SIGTERMハンドラ (async-signal-safeにself-pipeへ1byte書き込むのみ)
 * @param signalNumber 受信したシグナル番号 (未使用)
 */
void handleSigTerm(int signalNumber)
{
    Q_UNUSED(signalNumber)
    const char marker = 1;
    ::write(g_sigtermFd[1], &marker, sizeof(marker));
}

/**
 * @brief 追記モードで1行をログファイルへ書き込みflushする
 * @param path ログファイルの絶対path (空なら何もしない)
 * @param line 追記する1行 (改行は本関数が付与する)
 */
void appendLogLine(const QString &path, const QString &line)
{
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << line << Qt::endl;
    stream.flush();
    file.flush();
}

} // namespace

/**
 * @brief SIGTERMをQt event loopの終了要求へ橋渡しするhelper
 *
 * self-pipe trickを使い、シグナルハンドラ本体からはpipeへ1byte書き込むだけに
 * 留め、実際のQCoreApplication::quit()呼び出しはQSocketNotifier経由で
 * event loop context上で行う。
 */
class SigTermBridge : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief self-pipeとSIGTERMハンドラを設定する
     * @param parent 親QObject
     */
    explicit SigTermBridge(QObject *parent = nullptr)
        : QObject(parent)
    {
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigtermFd);
        m_notifier = new QSocketNotifier(g_sigtermFd[0], QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &SigTermBridge::onActivated);

        struct sigaction action;
        action.sa_handler = handleSigTerm;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &action, nullptr);
    }

private slots:
    /**
     * @brief self-pipeが読み取り可能になったらevent loopを終了する
     */
    void onActivated()
    {
        m_notifier->setEnabled(false);
        char marker = 0;
        ::read(g_sigtermFd[0], &marker, sizeof(marker));
        QCoreApplication::quit();
    }

private:
    QSocketNotifier *m_notifier = nullptr;
};

/**
 * @brief staged restore 6メソッドのみを実security coreへ橋渡しするテスト専用D-Busサービス
 */
class RestorePlanTestService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

public:
    /**
     * @brief registryとexecutorを結線して構築する
     * @param parent 親QObject
     */
    explicit RestorePlanTestService(QObject *parent = nullptr)
        : QObject(parent)
        , m_restoreExecutor(m_restoreRegistry)
        , m_authLogPath(QString::fromLocal8Bit(qgetenv("QSNAPPER_TEST_AUTH_LOG")))
        , m_applyLogPath(QString::fromLocal8Bit(qgetenv("QSNAPPER_TEST_APPLY_LOG")))
    {
        m_restoreExecutor.setEntryApplier(
            [this](const QString &manifestId,
                   const qsnapper::restore::RestoreEntry &entry) {
                return applyRestoreEntryForTest(manifestId, entry);
            });
        m_restoreExecutor.setProgressSink(
            [this](const QString &manifestId, int current, int total,
                   const QString &path) {
                emit restorePlanProgress(manifestId, current, total,
                                         QFileInfo(path).fileName());
            });
        m_restoreExecutor.setFinishedSink(
            [this](const QString &manifestId,
                   qsnapper::restore::ManifestState terminal,
                   const QString &message) {
                finishRestorePlan(manifestId, terminal, message);
            });
        m_restoreExecutor.setChunkScheduler(
            [this](std::function<void()> chunk) {
                QTimer::singleShot(0, this, [chunk]() { chunk(); });
            });
    }

public slots:
    /**
     * @brief ownerに束縛された空のstaged restore計画を開始する
     *
     * snapshotoperations.cpp BeginRestorePlan (行1811-1865) と同じ順序:
     * owner確認 -> resolveConfigOrFail -> restoreMode検証 -> snapshotNumber検証
     * -> purgeExpired -> createStaging
     *
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元snapshot番号
     * @param restoreMode yastまたはdirect
     * @return 成功時manifest id
     */
    QString BeginRestorePlan(const QString &configName, int snapshotNumber,
                             const QString &restoreMode)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return {};
        }

        const auto cfg = resolveConfigOrFail(configName);
        if (!cfg) {
            return {};
        }

        qsnapper::restore::RestoreMode mode;
        if (restoreMode == QStringLiteral("yast")) {
            mode = qsnapper::restore::RestoreMode::YastCompatible;
        }
        else if (restoreMode == QStringLiteral("direct")) {
            mode = qsnapper::restore::RestoreMode::DirectCopy;
        }
        else {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Invalid restore mode"));
            return {};
        }

        if (snapshotNumber <= 0) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Invalid snapshot number"));
            return {};
        }

        purgeExpiredRestorePlansForTest();

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const QString manifestId = m_restoreRegistry.createStaging(
            owner, *cfg, snapshotNumber, mode, &error);
        if (manifestId.isEmpty()) {
            sendManifestError(error);
            return {};
        }

        m_restorePlanOwners.insert(manifestId, owner);
        return manifestId;
    }

    /**
     * @brief staging計画へ検証済みentry chunkを原子的に追加する
     *
     * snapshotoperations.cpp StageRestoreEntries (行1874-1933) と
     * entry検証ループをVERBATIMに再現する。
     *
     * @param manifestId owner束縛されたmanifest id
     * @param filePaths 復元対象絶対path列
     * @param changeTypes pathと対応する変更種別列
     * @return chunk全体を追加できた場合true
     */
    bool StageRestoreEntries(const QString &manifestId,
                             const QStringList &filePaths,
                             const QStringList &changeTypes)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return false;
        }

        if (filePaths.size() != changeTypes.size()) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Restore entry lists must have the same size"));
            return false;
        }
        if (filePaths.isEmpty()) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Restore entry chunk is empty"));
            return false;
        }
        if (filePaths.size()
                > qsnapper::restore::RestoreManifestRegistry::kMaxEntriesPerStageChunk) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Restore entry chunk is too large"));
            return false;
        }

        for (qsizetype index = 0; index < filePaths.size(); ++index) {
            const QString &path = filePaths.at(index);
            const QString &changeType = changeTypes.at(index);
            const bool validChangeType = changeType == QStringLiteral("created")
                    || changeType == QStringLiteral("deleted")
                    || changeType == QStringLiteral("modified")
                    || changeType == QStringLiteral("typechanged");
            QString relativePath;

            if (!path.startsWith(QLatin1Char('/'))
                    || path == QStringLiteral("/.snapshots")
                    || path.startsWith(QStringLiteral("/.snapshots/"))
                    || !validChangeType
                    || !qsnapper::security::splitDestinationBeneathRoot(
                        QStringLiteral("/"), path, &relativePath)) {
                sendErrorReply(QDBusError::InvalidArgs,
                               QStringLiteral("Invalid restore entry"));
                return false;
            }
        }

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        if (!m_restoreRegistry.stageEntries(manifestId, owner, filePaths,
                                            changeTypes, &error)) {
            return sendManifestError(error);
        }
        return true;
    }

    /**
     * @brief 計画をfreeze後に一度だけ認可し非同期実行を開始する
     *
     * snapshotoperations.cpp CommitRestorePlan (行1940-2068) と
     * 「freezeがauthorizationより前」「状態事前条件」「totalEntries<=0確認」の
     * 順序をVERBATIMに保つ。libsnapperでのmount (行1994-2055) はuser namespace内で
     * 実行できないため丸ごとskipし、freeze直後の認可のみ
     * authorizeForTest() へ置き換える。
     *
     * @param manifestId owner束縛されたmanifest id
     * @return 実行開始を受理した場合true
     */
    bool CommitRestorePlan(const QString &manifestId)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return false;
        }

        purgeExpiredRestorePlansForTest();

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const auto status = m_restoreRegistry.status(manifestId, owner, &error);
        if (!status) {
            return sendManifestError(error);
        }
        if (status->state == qsnapper::restore::ManifestState::Completed
                || status->state == qsnapper::restore::ManifestState::Failed
                || status->state == qsnapper::restore::ManifestState::Cancelled) {
            return sendManifestError(
                qsnapper::restore::ManifestError::AlreadyTerminal);
        }
        if (status->state != qsnapper::restore::ManifestState::Staging) {
            return sendManifestError(qsnapper::restore::ManifestError::WrongState);
        }
        if (status->totalEntries <= 0) {
            return sendManifestError(
                qsnapper::restore::ManifestError::InvalidArgument);
        }
        if (!m_restoreRegistry.freeze(manifestId, owner, &error)) {
            return sendManifestError(error);
        }

        // TEST-ONLY SUBSTITUTION: 本番はここで
        // checkAuthorization("com.presire.qsnapper.rollback-snapshot") を呼び
        // Polkitへ問い合わせる。user namespace内ではpolkitdへ到達できないため
        // authorizeForTest() へ置き換える (常にtrueを返しつつ認可回数を
        // QSNAPPER_TEST_AUTH_LOGへ記録する)。
        if (!authorizeForTest()) {
            qsnapper::restore::ManifestError failureError =
                qsnapper::restore::ManifestError::None;
            m_restoreRegistry.markFailed(
                manifestId, owner, QStringLiteral("Authorization failed"),
                &failureError);
            return false;
        }

        // TEST-ONLY SUBSTITUTION: 本番はここでgetSnapper()/mountFilesystemSnapshot()
        // によりlibsnapperで実snapshotをmountし、RestoreExecution contextを
        // m_restoreExecutionsへ保存する (snapshotoperations.cpp 行1986-2034)。
        // user namespace内ではbtrfs mount操作を実行できないため、本サービスは
        // mount手順を丸ごとskipしexecutorを直接startする。EntryApplierは
        // applyRestoreEntryForTest() (no-opスタブ) が処理する。

        if (!m_restoreExecutor.start(manifestId, owner, &error)) {
            qsnapper::restore::ManifestError failureError =
                qsnapper::restore::ManifestError::None;
            m_restoreRegistry.markFailed(
                manifestId, owner, QStringLiteral("Failed to start restore work"),
                &failureError);
            return sendManifestError(error);
        }

        return true;
    }

    /**
     * @brief owner確認済みRunning計画のidle loopを再開する
     * @param manifestId owner束縛されたmanifest id
     * @return nudgeを受理した場合true
     */
    bool ContinueRestorePlan(const QString &manifestId)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return false;
        }

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        if (!m_restoreExecutor.requestContinue(manifestId, owner, &error)) {
            return sendManifestError(error);
        }
        return true;
    }

    /**
     * @brief owner確認済み計画状態をRFC4180 escaping済みCSVで返す
     * @param manifestId owner束縛されたmanifest id
     * @return ManifestStatus field順のCSV、失敗時空文字列
     */
    QString GetRestorePlanStatus(const QString &manifestId)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return {};
        }

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const auto status = m_restoreRegistry.status(manifestId, owner, &error);
        if (!status) {
            sendManifestError(error);
            return {};
        }

        const QStringList fields{
            status->id,
            restoreManifestStateString(status->state),
            QString::number(status->totalEntries),
            QString::number(status->cursor),
            QString::number(status->processed),
            restoreModeString(status->mode),
            quoteRestoreStatusCsvField(status->configName),
            QString::number(status->snapshotNumber),
            quoteRestoreStatusCsvField(status->lastError)
        };
        return fields.join(QLatin1Char(','));
    }

    /**
     * @brief owner確認済み非終端計画へ境界cancellationを要求する
     * @param manifestId owner束縛されたmanifest id
     * @return cancellationを受理した場合true
     */
    bool CancelRestorePlan(const QString &manifestId)
    {
        const QString owner = callerOwner();
        if (calledFromDBus() && owner.isEmpty()) {
            sendErrorReply(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return false;
        }

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        if (!m_restoreExecutor.requestCancel(manifestId, owner, &error)) {
            return sendManifestError(error);
        }
        return true;
    }

signals:
    /**
     * @brief staged restore計画の進捗通知
     * @param manifestId 実行中計画id
     * @param current 完了entry数
     * @param total 凍結時の総entry数
     * @param filePath 情報漏洩を抑えたbasename
     */
    void restorePlanProgress(const QString &manifestId, int current, int total,
                             const QString &filePath);

    /**
     * @brief staged restore計画の終端通知
     * @param manifestId 終端した計画id
     * @param terminalState completed/failed/cancelledのいずれか
     * @param message 終端理由
     */
    void restorePlanFinished(const QString &manifestId, const QString &terminalState,
                             const QString &message);

private:
    /**
     * @brief 現在のD-Bus呼び出し元unique nameを返す
     * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
     */
    QString callerOwner() const
    {
        return calledFromDBus() ? message().service() : QString();
    }

    /**
     * @brief configNameを正規化＋検証し、不正ならD-Busエラー応答を返す
     * @param configName 検査対象の設定名 (空文字列は "root" として扱う)
     * @return 正規化後の設定名 (有効な場合)、無効でエラー応答済みなら std::nullopt
     */
    std::optional<QString> resolveConfigOrFail(const QString &configName)
    {
        const QString effective = configName.isEmpty() ? QStringLiteral("root") : configName;
        if (!qsnapper::security::validateConfigName(effective)) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("Invalid configName"));
            return std::nullopt;
        }
        return effective;
    }

    /**
     * @brief manifest操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
     *
     * snapshotoperations.cpp sendManifestError (行452-493) とbyte-identical。
     *
     * @param error registryが返したエラー
     * @return 常にfalse
     */
    bool sendManifestError(qsnapper::restore::ManifestError error)
    {
        using qsnapper::restore::ManifestError;

        QDBusError::ErrorType errorType = QDBusError::Failed;
        QString messageText = QStringLiteral("Restore plan operation failed");

        switch (error) {
        case ManifestError::NotFound:
        case ManifestError::OwnerMismatch:
        case ManifestError::Expired:
            errorType = QDBusError::AccessDenied;
            messageText = QStringLiteral("Restore plan access denied");
            break;
        case ManifestError::WrongState:
            errorType = QDBusError::Failed;
            messageText = QStringLiteral("Restore plan is not in the required state");
            break;
        case ManifestError::AlreadyTerminal:
            errorType = QDBusError::Failed;
            messageText = QStringLiteral("Restore plan is already terminal");
            break;
        case ManifestError::CapacityExceeded:
            errorType = QDBusError::InvalidArgs;
            messageText = QStringLiteral("Restore plan capacity exceeded");
            break;
        case ManifestError::GlobalLimit:
            errorType = QDBusError::LimitsExceeded;
            messageText = QStringLiteral("Restore plan limit exceeded");
            break;
        case ManifestError::InvalidArgument:
            errorType = QDBusError::InvalidArgs;
            messageText = QStringLiteral("Invalid restore plan request");
            break;
        case ManifestError::None:
            break;
        }

        sendErrorReply(errorType, messageText);
        return false;
    }

    /**
     * @brief manifest状態をD-Bus contractの小文字表現へ変換する
     * @param state 変換対象状態
     * @return contractで定義した状態文字列
     */
    static QString restoreManifestStateString(qsnapper::restore::ManifestState state)
    {
        using qsnapper::restore::ManifestState;

        switch (state) {
        case ManifestState::Staging:
            return QStringLiteral("staging");
        case ManifestState::Frozen:
            return QStringLiteral("frozen");
        case ManifestState::Running:
            return QStringLiteral("running");
        case ManifestState::Completed:
            return QStringLiteral("completed");
        case ManifestState::Failed:
            return QStringLiteral("failed");
        case ManifestState::Cancelled:
            return QStringLiteral("cancelled");
        }
        return QStringLiteral("failed");
    }

    /**
     * @brief 復元方式をD-Bus contractの文字列表現へ変換する
     * @param mode 変換対象方式
     * @return yastまたはdirect
     */
    static QString restoreModeString(qsnapper::restore::RestoreMode mode)
    {
        return mode == qsnapper::restore::RestoreMode::DirectCopy
            ? QStringLiteral("direct")
            : QStringLiteral("yast");
    }

    /**
     * @brief RFC4180形式で必要なCSV fieldをquoteする
     * @param field quote対象文字列
     * @return CSVへ安全に埋め込めるfield
     */
    static QString quoteRestoreStatusCsvField(const QString &field)
    {
        if (!field.contains(QLatin1Char(','))
                && !field.contains(QLatin1Char('"'))) {
            return field;
        }

        QString escaped = field;
        escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }

    /**
     * @brief TEST-ONLY SUBSTITUTION: checkAuthorizationの代替実装
     *
     * 本番のcheckAuthorization (snapshotoperations.cpp 行852以降) は
     * PolkitQt1::Authority::checkAuthorizationSync()でpolkitdへ問い合わせるが、
     * user namespace内ではpolkitdへ到達できない。本stubは常にtrueを返しつつ、
     * 認可が要求された回数をQSNAPPER_TEST_AUTH_LOGへ1行追記することで、
     * テストから「1回だけ認可されたこと」を観測可能にする。
     *
     * @return 常にtrue
     */
    bool authorizeForTest()
    {
        ++m_authorizationCount;
        appendLogLine(m_authLogPath,
                     QStringLiteral("AUTH %1").arg(m_authorizationCount));
        return true;
    }

    /**
     * @brief TEST-ONLY SUBSTITUTION: RestorePlanExecutor::EntryApplierの代替実装
     *
     * 本番のapplyRestoreEntry (snapshotoperations.cpp 行2681以降) は
     * libsnapperでmountしたsnapshotから実ファイルをlive filesystemへコピーするが、
     * user namespace内ではbtrfs mount操作を実行できない。本stubは
     * QSNAPPER_TEST_APPLY_LOGへentry pathを1行追記し、1ミリ秒sleepしてtrueを
     * 返すだけのno-opとする。
     *
     * @param manifestId 実行中manifest id (未使用)
     * @param entry 適用対象entry
     * @return 常にtrue
     */
    bool applyRestoreEntryForTest(const QString &manifestId,
                                  const qsnapper::restore::RestoreEntry &entry)
    {
        Q_UNUSED(manifestId)
        appendLogLine(m_applyLogPath, entry.path);
        QThread::msleep(1);
        return true;
    }

    /**
     * @brief 終端計画のsignal送出とregistry削除を実行する
     *
     * snapshotoperations.cpp finishRestorePlan (行628以降) からmount解除と
     * safety netを除いた部分をVERBATIMに再現する。
     *
     * @param manifestId 終端したmanifest id
     * @param terminal 終端状態
     * @param messageText 終端理由
     */
    void finishRestorePlan(const QString &manifestId,
                           qsnapper::restore::ManifestState terminal,
                           const QString &messageText)
    {
        emit restorePlanFinished(manifestId,
                                 restoreManifestStateString(terminal),
                                 messageText);

        m_restoreRegistry.remove(manifestId);
        m_restorePlanOwners.remove(manifestId);
    }

    /**
     * @brief TTL purgeで消えたactive計画をabandonする
     *
     * snapshotoperations.cpp purgeExpiredRestorePlans (行679以降) から
     * mount cleanupとowner watcher更新を除いた部分を再現する。
     */
    void purgeExpiredRestorePlansForTest()
    {
        m_restoreRegistry.purgeExpired();

        const QStringList planIds = m_restorePlanOwners.keys();
        for (const QString &manifestId : planIds) {
            qsnapper::restore::ManifestError error =
                qsnapper::restore::ManifestError::None;
            const QString owner = m_restorePlanOwners.value(manifestId);
            if (!m_restoreRegistry.status(manifestId, owner, &error)) {
                // executorが同じ計画をもう一度終端しないよう先にabandonしてから、
                // finishRestorePlanで終端して終端signalを必ず発火させる
                m_restoreExecutor.abandon(manifestId);
                finishRestorePlan(
                    manifestId, qsnapper::restore::ManifestState::Failed,
                    QStringLiteral("Restore plan expired before completion"));
            }
        }
    }

    qsnapper::restore::RestoreManifestRegistry m_restoreRegistry;
    qsnapper::restore::RestorePlanExecutor m_restoreExecutor;
    QMap<QString, QString> m_restorePlanOwners;
    const QString m_authLogPath;
    const QString m_applyLogPath;
    int m_authorizationCount = 0;
};

/**
 * @brief エントリポイント
 *
 * QSNAPPER_TEST_READY_FILEが設定されていれば、名前とオブジェクトの登録完了後に
 * "READY" を書き込みflushする。あわせて標準出力へ "SERVICE_READY" を出力しflushする。
 *
 * @param argc 引数の数
 * @param argv 引数配列
 * @return 終了コード
 */
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    SigTermBridge sigTermBridge;

    QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        std::fprintf(stderr, "ERROR: Cannot connect to the D-Bus system bus\n");
        return 1;
    }

    if (!connection.registerService(QStringLiteral("com.presire.qsnapper.Operations"))) {
        std::fprintf(stderr, "ERROR: Failed to register D-Bus service: %s\n",
                     qPrintable(connection.lastError().message()));
        return 1;
    }

    RestorePlanTestService service;
    if (!connection.registerObject(QStringLiteral("/com/presire/qsnapper/Operations"), &service,
                                   QDBusConnection::ExportAllSlots
                                       | QDBusConnection::ExportAllSignals)) {
        std::fprintf(stderr, "ERROR: Failed to register D-Bus object: %s\n",
                     qPrintable(connection.lastError().message()));
        return 1;
    }

    const QString readyFilePath = QString::fromLocal8Bit(qgetenv("QSNAPPER_TEST_READY_FILE"));
    if (!readyFilePath.isEmpty()) {
        QFile readyFile(readyFilePath);
        if (readyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            readyFile.write("READY");
            readyFile.flush();
            readyFile.close();
        }
    }
    std::fputs("SERVICE_READY\n", stdout);
    std::fflush(stdout);

    return app.exec();
}

#include "restoreplan_test_service.moc"
