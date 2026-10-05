/**
 * @file filechangemodel_loading.cpp
 * @brief 変更一覧の読み込みと比較条件の管理と差分取得
 *
 * D-Bus経由の非同期読み込みと世代による古い応答の破棄
 * およびComparisonContextの一致検証を担当する
 */

#include "filechangemodel.h"
#include "filechangemodel_p.h"
#include "filechangemodel_transport_p.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QFileInfo>
#include <QSettings>
#include <QElapsedTimer>
#include <QDebug>


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
 * @brief 文字列応答のD-Bus呼び出しを非同期で発行する
 *
 * @param methodName 呼び出すD-Busメソッド名
 * @param arguments メソッドへ渡す引数
 * @param done 結果コールバック
 */
void FileChangeModel::requestStringReply(const QString &methodName,
                                         const QVariantList &arguments,
                                         AsyncStringReplyHandler done)
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!reconnectDbus()) {
            done(false, QString(), QStringLiteral("D-Bus connection failed"));
            return;
        }
    }

    const QDBusPendingCall pendingCall = m_dbusInterface->asyncCallWithArgumentList(methodName, arguments);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [done](QDBusPendingCallWatcher *finishedWatcher) {
                QDBusPendingReply<QString> reply = *finishedWatcher;
                if (reply.isError()) {
                    done(false, QString(), reply.error().message());
                }
                else {
                    done(true, reply.value(), QString());
                }
                finishedWatcher->deleteLater();
            });
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
        invalidateLoadedComparison();
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
    const ComparisonContext previousContext = currentComparisonContext();
    if (m_snapshotNumber != number) {
        m_snapshotNumber = number;
        emit snapshotNumberChanged();
    }
    // snapshotNumber を明示セットした場合は「対カレント比較」モードに戻す
    m_betweenMode = false;
    m_flatMode    = false;
    m_filterUnchangedAgainstCurrent = false;
    if (!(previousContext == currentComparisonContext())) {
        invalidateLoadedComparison();
    }
}

/**
 * @brief ファイル変更リストを読み込み (非同期) 
 *
 * D-Bus経由でSnapperからファイル変更リストを非同期で取得し、モデルを構築する
 * 読み込み中はloadingプロパティがtrueになる
 */
void FileChangeModel::loadChanges()
{
    // loadChangesBetween()は、呼び出し側で比較条件を設定してからこの関数を呼ぶ
    if (m_configName.isEmpty()
            || (!m_betweenMode && m_snapshotNumber <= 0)
            || (m_betweenMode && (m_compareNumber1 <= 0 || m_compareNumber2 <= 0))) {
        qWarning() << "Invalid config name or snapshot number:" << m_configName << m_snapshotNumber;
        emit errorOccurred("Invalid config name or snapshot number");
        return;
    }

    // 新しい要求を始める前に、失敗しても旧方向のエントリを復元できない状態にする
    invalidateLoadedComparison();
    const ComparisonContext requestedContext = currentComparisonContext();
    const quint64 requestGeneration = m_loadRequestGeneration;

    m_loading = true;
    emit loadingChanged();

    // 現在のシステムと差異が無いエントリを除外する場合は、2つの比較結果を合流させてから公開する
    if (requestedContext.filterUnchangedAgainstCurrent) {
        requestFilteredChangeList(requestedContext, requestGeneration);
        return;
    }

    const auto requestTimer = QSharedPointer<QElapsedTimer>::create();
    requestTimer->start();

    const QString methodName = requestedContext.betweenMode
        ? QStringLiteral("GetFileChangesBetween")
        : QStringLiteral("GetFileChanges");
    QVariantList arguments;
    arguments << requestedContext.configName;
    if (requestedContext.betweenMode) {
        arguments << requestedContext.number1 << requestedContext.number2;
    }
    else {
        arguments << requestedContext.number1;
    }

    requestStringReply(methodName, arguments,
                       [this, requestedContext, requestGeneration, requestTimer](bool ok,
                                                                                   const QString &output,
                                                                                   const QString &error) {
        // 条件変更または後続要求の開始後に届いた応答は、モデルへ絶対に公開しない
        if (requestGeneration != m_loadRequestGeneration
                || !(requestedContext == currentComparisonContext())) {
            return;
        }

        if (!ok) {
            qWarning() << "Failed to get file changes via D-Bus:" << error;
            m_loading = false;
            emit loadingChanged();
            emit errorOccurred(QStringLiteral("Failed to get file changes: %1").arg(error));
            return;
        }

        if (output.isEmpty()) {
            qWarning() << "snapper status command returned empty output";
            m_loading = false;
            emit loadingChanged();
            emit errorOccurred("No file changes found");
            return;
        }

        setupModelData(output, requestedContext.flatMode);
        if (!m_hasChanges) {
            m_hasChanges = true;
            emit hasChangesChanged();
        }
        qInfo() << "FileChangeModel timing: dbusWait=" << requestTimer->elapsed() << "ms";

        m_loading = false;
        emit loadingChanged();
    });
}

/**
 * @brief 現在のシステムと差異が無いエントリを除いた変更一覧を読み込む
 *
 * 「比較元 --> 現在のシステム」を先に取得し、その応答を受け取ってから「比較元 --> 比較先」を取得して、後者を前者のパス集合で絞り込んでからモデルへ公開する
 * 公開されるエントリは常に後者の部分集合であり、エントリの変更種別も書き換えないため、この絞り込みが復元対象を広げることはない
 *
 * 2つの比較を並行させず直列化するのには理由が2つある
 * 1. 両メソッドは同じPolkit action (view-diff) で個別に認可される
 *    並行させると、初回の認可がキャッシュされる前に2件の認可要求が同時に飛び、認証プロンプトが二重に出る可能性がある
 * 2. サービス側のComparisonキャッシュは1件しか保持しないため、直列化することで「比較元 --> 比較先」のComparisonが最後に残ることが確定し、
 *    後続のファイル差分取得が再利用できる
 *    サービス側の比較処理自体は単一のイベントループ上で直列に実行されるため、並行要求にしても所要時間は短縮されない
 *
 * @param requestedContext この要求を発行した時点の比較条件
 * @param requestGeneration この要求の世代
 */
void FileChangeModel::requestFilteredChangeList(const ComparisonContext &requestedContext,
                                                quint64 requestGeneration)
{
    const auto requestTimer = QSharedPointer<QElapsedTimer>::create();
    requestTimer->start();

    QVariantList currentArguments;
    currentArguments << requestedContext.configName << requestedContext.number1;

    requestStringReply(QStringLiteral("GetFileChanges"), currentArguments,
                       [this, requestedContext, requestGeneration, requestTimer](bool ok,
                                                                                   const QString &currentOutput,
                                                                                   const QString &error) {
        // 条件変更または後続要求の開始後に届いた応答は、モデルへ絶対に公開しない
        // 後続の「比較元 --> 比較先」要求もここで打ち切る
        if (requestGeneration != m_loadRequestGeneration
                || !(requestedContext == currentComparisonContext())) {
            return;
        }

        if (!ok) {
            qWarning() << "Failed to get file changes via D-Bus:" << error;
            m_loading = false;
            emit loadingChanged();
            emit errorOccurred(QStringLiteral("Failed to get file changes: %1").arg(error));
            return;
        }

        QVariantList betweenArguments;
        betweenArguments << requestedContext.configName << requestedContext.number1 << requestedContext.number2;

        requestStringReply(QStringLiteral("GetFileChangesBetween"), betweenArguments,
                           [this, requestedContext, requestGeneration, requestTimer, currentOutput](bool betweenOk,
                                                                                                    const QString &betweenOutput,
                                                                                                    const QString &betweenError) {
            if (requestGeneration != m_loadRequestGeneration
                    || !(requestedContext == currentComparisonContext())) {
                return;
            }

            if (!betweenOk) {
                qWarning() << "Failed to get file changes via D-Bus:" << betweenError;
                m_loading = false;
                emit loadingChanged();
                emit errorOccurred(QStringLiteral("Failed to get file changes: %1").arg(betweenError));
                return;
            }

            publishFilteredChangeList(requestedContext, betweenOutput, currentOutput);
            qInfo() << "FileChangeModel timing: dbusWait=" << requestTimer->elapsed() << "ms";
        });
    });
}

/**
 * @brief 絞り込んだ変更一覧をモデルへ公開する
 *
 * 絞り込みの結果が空になるのは「比較元と現在のシステムに差異がもう無い = 全て復元済み」という
 * 正常な状態であるため、エラーとして扱わず「差分なし」表示へ落とす
 *
 * @param requestedContext この要求を発行した時点の比較条件
 * @param betweenOutput 比較元 --> 比較先 の変更一覧
 * @param currentOutput 比較元 --> 現在のシステム の変更一覧
 */
void FileChangeModel::publishFilteredChangeList(const ComparisonContext &requestedContext,
                                                const QString &betweenOutput,
                                                const QString &currentOutput)
{
    const QSet<QString> pathsDifferingFromCurrent = collectChangedPaths(currentOutput);
    const QString filteredOutput = filterChangeOutputByPaths(betweenOutput, pathsDifferingFromCurrent);

    setupModelData(filteredOutput, requestedContext.flatMode);

    const bool hasChanges = m_rootItem != nullptr && m_rootItem->childCount() > 0;
    if (m_hasChanges != hasChanges) {
        m_hasChanges = hasChanges;
        emit hasChangesChanged();
    }

    m_loading = false;
    emit loadingChanged();
}

/**
 * @brief 任意の2つのスナップショット間のファイル変更を読み込む
 *
 * num1 --> num2 の差分を取得し、ツリー構造を構築する
 * 取得したエントリのchangeTypeはnum1 --> num2の向きで解釈されるため、
 * このモードから復元する場合の復元元はnum1に限られる (currentRestoreSourceNumber()参照)
 *
 * @param number1 比較元スナップショット番号 (このモードでの復元元)
 * @param number2 比較先スナップショット番号
 * @param flat trueの場合はフラットモデルを構築する
 * @param filterUnchangedAgainstCurrent trueの場合、number1と現在のシステムの間に差異が無いエントリを除外する
 */
void FileChangeModel::loadChangesBetween(int number1, int number2, bool flat,
                                         bool filterUnchangedAgainstCurrent)
{
    if (m_configName.isEmpty() || number1 <= 0 || number2 <= 0) {
        emit errorOccurred("Invalid config name or snapshot numbers");
        return;
    }

    setComparisonRange(number1, number2, flat, filterUnchangedAgainstCurrent);
    loadChanges();
}

/**
 * @brief 2スナップショット比較の条件を設定する
 *
 * @param number1 比較元スナップショット番号 (このモードでの復元元)
 * @param number2 比較先スナップショット番号
 * @param flat trueの場合はフラットモデルを構築する
 * @param filterUnchangedAgainstCurrent trueの場合、number1と現在のシステムの間に差異が無いエントリを除外する
 */
void FileChangeModel::setComparisonRange(int number1, int number2, bool flat,
                                         bool filterUnchangedAgainstCurrent)
{
    const ComparisonContext previousContext = currentComparisonContext();
    m_betweenMode     = true;
    m_flatMode        = flat;  // true = フラット (比較ダイアログ), false = ツリー (復元プレビュー)
    m_compareNumber1  = number1;
    m_compareNumber2  = number2;
    m_filterUnchangedAgainstCurrent = filterUnchangedAgainstCurrent;

    // 対カレント比較へ戻った際の既定表示に備えて、比較先スナップショットを保持しておく
    // 復元元としては使用しない (このモードの復元元はcurrentRestoreSourceNumber()がm_compareNumber1を返す)
    m_snapshotNumber  = number2;
    if (!(previousContext == currentComparisonContext())) {
        invalidateLoadedComparison();
    }
}

/**
 * @brief 比較条件が完全に一致するかを返す
 *
 * @param other 比較対象の比較条件
 * @return 全ての項目が一致する場合: true
 */
bool FileChangeModel::ComparisonContext::operator==(const ComparisonContext &other) const
{
    return configName  == other.configName
        && betweenMode == other.betweenMode
        && number1     == other.number1
        && number2     == other.number2
        && flatMode    == other.flatMode
        && filterUnchangedAgainstCurrent == other.filterUnchangedAgainstCurrent;
}

/**
 * @brief 現在の比較条件を返す
 *
 * number1には常に、その比較の向きにおける復元元スナップショット番号を入れる
 *
 * @return 現在の比較条件
 */
FileChangeModel::ComparisonContext FileChangeModel::currentComparisonContext() const
{
    ComparisonContext context;
    context.configName  = m_configName;
    context.betweenMode = m_betweenMode;
    context.number1     = m_betweenMode ? m_compareNumber1 : m_snapshotNumber;
    context.number2     = m_betweenMode ? m_compareNumber2 : 0;
    context.flatMode    = m_flatMode;
    context.filterUnchangedAgainstCurrent = m_betweenMode && m_filterUnchangedAgainstCurrent;
    return context;
}

/**
 * @brief 公開済みエントリとその由来を無効化する
 *
 * 比較条件を変更した時点、および新しい読み込みを開始した時点で呼び出す
 * これにより、置き換え先の読み込みが失敗しても古いエントリがモデルに残らず、
 * 新しい復元元と古いchangeTypeが結び付いてlive側のファイルを誤って削除する事故を防ぐ
 */
void FileChangeModel::invalidateLoadedComparison()
{
    m_loadedComparisonValid = false;
    m_loadedComparison      = ComparisonContext();
    ++m_loadRequestGeneration;

    // 進行中のファイル差分要求も無効化し、古い向きのdiffが新しいビューへ届かないようにする
    ++m_diffRequestGeneration;

    beginResetModel();
    clearModel();
    endResetModel();

    if (m_hasChanges) {
        m_hasChanges = false;
        emit hasChangesChanged();
    }

    if (m_loading) {
        m_loading = false;
        emit loadingChanged();
    }
}

/**
 * @brief 公開済みエントリから復元してよいかを返す
 *
 * 読み込みに成功したエントリであること、その由来が現在の比較条件と完全に一致すること、
 * 読み込み中でないこと、実際にエントリが存在することを全て要求する
 *
 * @return 復元を許可してよい場合: true
 */
bool FileChangeModel::hasRestorableLoadedComparison() const
{
    return m_loadedComparisonValid
        && !m_loading
        && m_loadedComparison == currentComparisonContext()
        && m_rootItem != nullptr
        && m_rootItem->childCount() > 0;
}

/**
 * @brief 現在の比較条件に対応する復元元スナップショット番号を返す
 *
 * モデルが保持するエントリのchangeTypeは、読み込み時の比較の向きでしか正しく解釈できない
 * 2スナップショット比較モードでは、loadChangesBetween()の第1引数 (m_compareNumber1) が復元元となる
 * この向きではcreatedが「比較先にだけ存在する = 復元元には無い」を意味し、live側からの削除と一致する
 * 対カレント比較モードでは、エントリはm_snapshotNumberと現在のシステムの差分であるため、
 * 復元元はm_snapshotNumber以外にはなり得ない
 *
 * @return 復元元として使用すべきスナップショット番号
 */
int FileChangeModel::currentRestoreSourceNumber() const
{
    return currentComparisonContext().number1;
}

/**
 * @brief 復元元と公開済みエントリの由来の整合性を検証する
 *
 * 呼び出し側が指定した復元元が、実際にモデルへ公開されたエントリの由来と一致するかを確認する
 * 不一致のまま復元するとcreatedとdeletedの意味が反転し、
 * 復元対象外のファイルをlive側から削除する事故につながるため、ここで遮断する
 *
 * @param sourceSnapshotNumber 復元元スナップショット番号
 * @return 復元元として使用できる場合: true
 */
bool FileChangeModel::isRestoreSourceConsistent(int sourceSnapshotNumber) const
{
    return sourceSnapshotNumber > 0
        && hasRestorableLoadedComparison()
        && sourceSnapshotNumber == m_loadedComparison.number1;
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

    const ComparisonContext requestedContext = currentComparisonContext();
    if ((!requestedContext.betweenMode && requestedContext.number1 <= 0)
            || (requestedContext.betweenMode
                && (requestedContext.number1 <= 0 || requestedContext.number2 <= 0))) {
        return;
    }

    const quint64 requestGeneration = ++m_diffRequestGeneration;
    const QString methodName = requestedContext.betweenMode
        ? QStringLiteral("GetFileDiffBetween")
        : QStringLiteral("GetFileDiffAndDetails");
    QVariantList arguments;
    arguments << requestedContext.configName;
    if (requestedContext.betweenMode) {
        arguments << requestedContext.number1 << requestedContext.number2 << filePath;
    }
    else {
        arguments << requestedContext.number1 << filePath;
    }

    requestStringReply(methodName, arguments,
                       [this, filePath, requestedContext, requestGeneration](bool ok,
                                                                               const QString &result,
                                                                               const QString &error) {
        if (requestGeneration != m_diffRequestGeneration
                || !(requestedContext == currentComparisonContext())) {
            return;
        }

        if (!ok) {
            qWarning() << "Failed to get file diff and details:" << error;
            emit fileDiffAndDetailsReady(filePath, QVariantMap(), QString());
            return;
        }

        if (result.isEmpty()) {
            emit fileDiffAndDetailsReady(filePath, QVariantMap(), QString());
            return;
        }

        // セパレータでdetails部とdiff部を分割
        const QString separator = "---DIFF_SEPARATOR---\n";
        const int sepIndex = result.indexOf(separator);

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
            const int eqPos = line.indexOf('=');
            if (eqPos > 0) {
                details[line.left(eqPos)] = line.mid(eqPos + 1);
            }
        }

        emit fileDiffAndDetailsReady(filePath, details, diffPart);
    });
}

/**
 * @brief 変更レコード出力から正規化済みパスの集合を抽出する
 *
 * @param changeOutput Snapperの変更出力
 * @return 出力に含まれる正規化済みパスの集合
 */
QSet<QString> FileChangeModel::collectChangedPaths(const QString &changeOutput)
{
    QSet<QString> paths;
    const QStringList lines = changeOutput.split('\n', Qt::SkipEmptyParts);
    paths.reserve(lines.size());

    for (const QString &line : lines) {
        filechangemodel_detail::ChangeInfo info;
        if (!filechangemodel_detail::parseChangeRecord(line, &info)) {
            continue;
        }
        // parseChangeRecord()が末尾スラッシュを除去するため、ディレクトリが一方の比較でのみ
        // 末尾スラッシュ付きで報告されても同一パスとして突き合わせられる
        paths.insert(info.path);
    }

    return paths;
}

/**
 * @brief 変更レコード出力を、許可パス集合に含まれる行だけへ絞り込む
 *
 * @param changeOutput 絞り込み対象のSnapperの変更出力
 * @param allowedPaths 表示を許可する正規化済みパスの集合
 * @return 絞り込み後の変更出力
 */
QString FileChangeModel::filterChangeOutputByPaths(const QString &changeOutput,
                                                   const QSet<QString> &allowedPaths)
{
    if (allowedPaths.isEmpty()) {
        return QString();
    }

    QString filteredOutput;
    filteredOutput.reserve(changeOutput.size());

    const QStringList lines = changeOutput.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        filechangemodel_detail::ChangeInfo info;
        // 行はステータスフラグごと元のまま転記する
        // 変更種別を書き換えないことで、復元時のcreated (削除) / deleted (コピー) の解釈が絞り込み前と完全に一致する
        if (!filechangemodel_detail::parseChangeRecord(line, &info) || !allowedPaths.contains(info.path)) {
            continue;
        }
        filteredOutput += line;
        filteredOutput += QLatin1Char('\n');
    }

    return filteredOutput;
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
