#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <snapper/Version.h>
#include <btrfsutil.h>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utime.h>
#include "snapshotoperations.h"
#include "inputvalidator.h"

// 古いlibsnapper (7.x未満) には LIBSNAPPER_VERSION_AT_LEAST マクロが存在しない
#ifndef LIBSNAPPER_VERSION_AT_LEAST
#define LIBSNAPPER_VERSION_AT_LEAST(major, minor)                                            \
    ((LIBSNAPPER_VERSION_MAJOR > (major)) ||                                                 \
     (LIBSNAPPER_VERSION_MAJOR == (major) && LIBSNAPPER_VERSION_MINOR >= (minor)))
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
#include <snapper/Plugins.h>
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
static void logPluginReport(const snapper::Plugins::Report& report)
{
    for (const auto& entry : report.entries) {
        if (entry.exit_status != 0) {
            qWarning() << "Snapper plugin" << QString::fromStdString(entry.name)
                       << "exited with status" << entry.exit_status;
        }
    }
}
#endif

// ============================================================================
// In-process unified diff (Myers diff algorithm)
// "diff -u"コマンドの置き換え
// ============================================================================

namespace {

struct DiffOp {
    enum Type { Equal, Delete, Insert };
    Type type;
    int aIdx, bIdx;  // 0-based index into old/new lines (-1 if N/A)
};

/**
 * Myers diffアルゴリズムで2つの文字列リスト間の最短編集スクリプトを計算する。
 *
 * @param a 旧ファイルの行リスト
 * @param b 新ファイルの行リスト
 * @return 編集操作のリスト (正順)
 */
static QVector<DiffOp> computeMyersDiff(const QStringList &a, const QStringList &b)
{
    const int N = a.size(), M = b.size();

    if (N == 0 && M == 0) return {};
    if (N == 0) {
        QVector<DiffOp> r;
        r.reserve(M);
        for (int i = 0; i < M; i++)
            r.append({DiffOp::Insert, -1, i});
        return r;
    }
    if (M == 0) {
        QVector<DiffOp> r;
        r.reserve(N);
        for (int i = 0; i < N; i++)
            r.append({DiffOp::Delete, i, -1});
        return r;
    }

    const int MAX = N + M, OFF = MAX;

    // V[k + OFF] = 対角線k上の最遠到達x座標
    QVector<int> V(2 * MAX + 1, 0);

    // 各dステップのVスナップショット (バックトラック用)
    QVector<QVector<int>> trace;
    trace.reserve(qMin(MAX, N + M));

    for (int d = 0; d <= MAX; d++) {
        trace.append(V);  // dステップ開始前 (= d-1ステップ終了後) のスナップショット
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && V[OFF + k - 1] < V[OFF + k + 1]))
                    ? V[OFF + k + 1] : V[OFF + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && a[x] == b[y]) {
                x++; y++;
            }
            V[OFF + k] = x;
            if (x >= N && y >= M) goto done;
        }
    }

done:
    // バックトラックで編集スクリプトを逆順に構築
    {
        QVector<DiffOp::Type> revTypes;
        revTypes.reserve(N + M);
        int x = N, y = M;

        for (int d = trace.size() - 1; d > 0; d--) {
            const QVector<int> &vp = trace[d];  // d-1ステップ終了後のV
            int k = x - y;
            bool down = (k == -d) || (k != d && vp[OFF + k - 1] < vp[OFF + k + 1]);
            int pk = down ? k + 1 : k - 1;
            int px = vp[OFF + pk], py = px - pk;
            int mx = down ? px : px + 1, my = mx - k;

            // 対角線上の等号行 (snake) を逆順に記録
            while (x > mx && y > my) {
                x--; y--;
                revTypes.append(DiffOp::Equal);
            }

            // 非対角移動 (挿入/削除)
            revTypes.append(down ? DiffOp::Insert : DiffOp::Delete);
            x = px; y = py;
        }

        // d=0の初期 snake (等号行のみ、編集なし)
        while (x > 0 && y > 0) {
            x--; y--;
            revTypes.append(DiffOp::Equal);
        }

        // 正順に反転
        std::reverse(revTypes.begin(), revTypes.end());

        // 操作タイプからインデックス付きDiffOpに変換
        QVector<DiffOp> result;
        result.reserve(revTypes.size());
        int ai = 0, bi = 0;
        for (auto t : revTypes) {
            switch (t) {
                case DiffOp::Equal:
                    result.append({DiffOp::Equal, ai, bi}); ai++; bi++; break;
                case DiffOp::Delete:
                    result.append({DiffOp::Delete, ai, -1}); ai++; break;
                case DiffOp::Insert:
                    result.append({DiffOp::Insert, -1, bi}); bi++; break;
            }
        }
        return result;
    }
}

/**
 * 2つのファイルを読み込み、unified diff形式の文字列を生成する。
 * "diff -u"コマンドと互換性のあるフォーマットで、QMLのformatDiffHtml()でパース可能。
 *
 * @param oldPath 旧ファイルパス (--- ヘッダに使用)
 * @param newPath 新ファイルパス (+++ ヘッダに使用)
 * @return unified diff文字列、差分がない場合は空文字列
 */
static QString generateUnifiedDiff(const QString &oldPath, const QString &newPath)
{
    QFile oldFile(oldPath), newFile(newPath);
    if (!oldFile.open(QIODevice::ReadOnly | QIODevice::Text) ||
        !newFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList a = QString::fromUtf8(oldFile.readAll()).split('\n');
    QStringList b = QString::fromUtf8(newFile.readAll()).split('\n');
    oldFile.close();
    newFile.close();

    // ファイル末尾の改行で生じる空要素を除去
    if (!a.isEmpty() && a.last().isEmpty()) a.removeLast();
    if (!b.isEmpty() && b.last().isEmpty()) b.removeLast();

    QVector<DiffOp> ops = computeMyersDiff(a, b);

    // 変更がない場合は空文字列を返す (diff -u の差分なしと同じ挙動)
    bool hasChanges = false;
    for (const auto &op : ops) {
        if (op.type != DiffOp::Equal) { hasChanges = true; break; }
    }
    if (!hasChanges) return {};

    // 変更位置を特定
    const int context = 3;
    QVector<int> changes;
    for (int i = 0; i < ops.size(); i++) {
        if (ops[i].type != DiffOp::Equal) changes.append(i);
    }

    // hunkにグループ化 (距離が2*context以内の変更をマージ)
    struct Hunk { int start, end; };
    QVector<Hunk> hunks;
    int hs = changes[0], he = changes[0];
    for (int i = 1; i < changes.size(); i++) {
        if (changes[i] - he <= 2 * context)
            he = changes[i];
        else {
            hunks.append({hs, he});
            hs = he = changes[i];
        }
    }
    hunks.append({hs, he});

    // unified diff形式で出力
    QString out;
    out += "--- " + oldPath + "\n";
    out += "+++ " + newPath + "\n";

    for (const auto &h : hunks) {
        int s = qMax(0, h.start - context);
        int e = qMin(ops.size() - 1, h.end + context);

        // hunk前の行数をカウント (行番号計算用)
        int aBefore = 0, bBefore = 0;
        for (int i = 0; i < s; i++) {
            if (ops[i].type != DiffOp::Insert) aBefore++;
            if (ops[i].type != DiffOp::Delete) bBefore++;
        }

        // hunk内の行数をカウント
        int aCount = 0, bCount = 0;
        for (int i = s; i <= e; i++) {
            if (ops[i].type != DiffOp::Insert) aCount++;
            if (ops[i].type != DiffOp::Delete) bCount++;
        }

        // 行番号は1ベース、空hunkの場合は0
        out += QString("@@ -%1,%2 +%3,%4 @@\n")
            .arg(aCount == 0 ? 0 : aBefore + 1).arg(aCount)
            .arg(bCount == 0 ? 0 : bBefore + 1).arg(bCount);

        for (int i = s; i <= e; i++) {
            switch (ops[i].type) {
                case DiffOp::Equal:
                    out += " " + a[ops[i].aIdx] + "\n";
                    break;
                case DiffOp::Delete:
                    out += "-" + a[ops[i].aIdx] + "\n";
                    break;
                case DiffOp::Insert:
                    out += "+" + b[ops[i].bIdx] + "\n";
                    break;
            }
        }
    }

    return out;
}

} // anonymous namespace

/**
 * @brief SnapshotOperationsクラスのコンストラクタ
 *
 * スナップショット操作を管理するクラスを初期化します。
 *
 * @param parent 親QObjectポインタ
 */
SnapshotOperations::SnapshotOperations(QObject *parent)
    : QObject(parent)
    , m_snapper(nullptr)
    , m_currentConfig("")
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(IdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, []() {
        qInfo() << "Idle timeout reached, shutting down...";
        QCoreApplication::quit();
    });
    m_idleTimer.start();
}

/**
 * @brief SnapshotOperationsクラスのデストラクタ
 *
 * リソースのクリーンアップを行います。
 */
SnapshotOperations::~SnapshotOperations()
{
}

/**
 * @brief アイドルタイマをリセット
 *
 * D-Busメソッド呼び出し時にタイマをリセットし、アイドルタイムアウトを延長します。
 */
void SnapshotOperations::resetIdleTimer()
{
    m_idleTimer.start();
}

/**
 * @brief Snapperが設定されているか確認
 *
 * Snapper設定が1つ以上存在するかを確認します。
 * 認証は不要 (list-snapshotsと同じアクションでactiveユーザは自動許可)
 *
 * @return Snapper設定が存在する場合: true
 */
bool SnapshotOperations::IsConfigured()
{
    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        return !configList.empty();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to check if snapper is configured:" << e.what();
        return false;
    }
}

/**
 * @brief Snapper設定を書き込む
 *
 * 指定されたキー/バリューペアをSnapper設定に書き込みます。
 * PolicyKit認証を必要とします。
 *
 * @param configName Snapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::WriteSnapperConfig(const QString &configName,
                                            const QMap<QString, QString> &settings)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    if (!checkAuthorization("com.presire.qsnapper.configure")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        std::map<std::string, std::string> info;
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info[it.key().toStdString()] = it.value().toStdString();
        }

        snapper->setConfigInfo(info);
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to write snapper config:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to write config: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief Snapperのクォータを設定
 *
 * 指定されたSnapper設定のクォータ機能を設定します。
 * PolicyKit認証を必要とします。
 *
 * @param configName Snapper設定名
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::SetupQuota(const QString &configName)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }
    if (!checkAuthorization("com.presire.qsnapper.configure")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper->setupQuota();
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to setup quota:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to setup quota: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief configNameを正規化＋検証し、不正ならD-Busエラー応答を送信する
 *
 * 空文字列入力を "root" に正規化した上で qsnapper::security::validateConfigName で検証する。
 * 無効な場合はQDBusError::InvalidArgsを送信し、std::nulloptを返す。
 * Polkitプロンプトを出す前に呼び出して、攻撃者が任意configNameでpolkitを浪費するのを防ぐ。
 *
 * 旧 validateConfigOrFail のリプレースメント。
 * 「空 → "root"」のデフォルト割当をここに集約することで、呼び出し側の
 * `configName.isEmpty() ? "root" : configName` パターンを排除する。
 *
 * @param configName 検査する設定名 (空文字列は "root" として扱う)
 * @return 正規化後の設定名 (有効時)、無効でエラー送信済み (std::nullopt)
 */
std::optional<QString> SnapshotOperations::resolveConfigOrFail(const QString &configName)
{
    const QString effective = configName.isEmpty()
                                 ? QStringLiteral("root")
                                 : configName;
    if (!qsnapper::security::validateConfigName(effective)) {
        sendErrorReply(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid configName"));
        return std::nullopt;
    }
    return effective;
}

/**
 * @brief PolicyKitによる認証チェックを実行
 *
 * 指定されたアクションIDに対してユーザが権限を持っているかを確認します。
 * 権限がない場合はD-Busエラー応答を送信します。
 *
 * SubjectはSystemBusNameSubjectを用いる。
 * UnixProcessSubject (PIDベース) はPIDがレース中に再割り当てされるTOCTOU脆弱性 (CVE-2013-4288) があり、polkit自身も非推奨としている。
 * SystemBusNameSubjectはカーネルのD-Bus name-owner情報をpolkitdが参照するため、呼び出し元の取り違えが起きない。
 *
 * @param actionId チェックするアクションID
 * @return 認証成功時: true、失敗時: false
 */
bool SnapshotOperations::checkAuthorization(const QString &actionId)
{
    resetIdleTimer();

    PolkitQt1::SystemBusNameSubject subject(message().service());
    PolkitQt1::Authority::Result result = PolkitQt1::Authority::instance()->checkAuthorizationSync(
        actionId, subject, PolkitQt1::Authority::AllowUserInteraction);

    if (result == PolkitQt1::Authority::Yes) {
        return true;
    }

    sendErrorReply(QDBusError::AccessDenied, "Authorization failed");
    return false;
}

/**
 * @brief Snapperインスタンスを取得
 *
 * 指定された設定名でSnapperインスタンスを取得または作成します。
 * 設定が変更された場合は新しいインスタンスを作成します。
 *
 * @param configName Snapper設定名
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 */
snapper::Snapper* SnapshotOperations::getSnapper(const QString &configName, bool forceReload)
{
    try {
        // 設定変更時・初回・強制リロード指定時に新しいSnapperインスタンスを作成する
        // libsnapperのSnapperオブジェクトは構築時にスナップショット一覧を読み込み、
        // 外部で作成された新規スナップショットを自動で取り込まないため、一覧更新時にはforceReloadでインスタンスを作り直す必要がある
        if (!m_snapper || m_currentConfig != configName || forceReload) {
            m_snapper.reset(new snapper::Snapper(configName.toStdString(), "/"));
            m_currentConfig = configName;
        }
        return m_snapper.get();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to create Snapper instance:" << e.what();
        return nullptr;
    }
}

/**
 * @brief スナップショットタイプを文字列に変換
 *
 * snapperライブラリのスナップショットタイプ列挙値を文字列表現に変換します。
 *
 * @param type スナップショットタイプ (snapper::SINGLE, PRE, POST)
 * @return タイプの文字列表現 ("single", "pre", "post")
 */
QString SnapshotOperations::snapshotTypeToString(int type)
{
    switch (type) {
        case snapper::SINGLE: return "single";
        case snapper::PRE: return "pre";
        case snapper::POST: return "post";
        default: return "single"    ;
    }
}

/**
 * @brief 文字列をスナップショットタイプに変換
 *
 * 文字列表現をsnapperライブラリのスナップショットタイプ列挙値に変換します。
 *
 * @param typeStr タイプの文字列表現 ("single", "pre", "post")
 * @return スナップショットタイプ列挙値
 */
int SnapshotOperations::stringToSnapshotType(const QString &typeStr)
{
    if (typeStr == "pre") return snapper::PRE;
    if (typeStr == "post") return snapper::POST;
    return snapper::SINGLE;
}

/**
 * @brief スナップショット一覧をCSV形式に変換
 *
 * Snapperインスタンスから取得したスナップショット一覧をCSV形式の文字列に変換します。
 *
 * @param snapper Snapperインスタンスへのポインタ
 * @return CSV形式のスナップショット情報文字列
 */
QString SnapshotOperations::formatSnapshotToCSV(const snapper::Snapper *snapper)
{
    if (!snapper) {
        return QString();
    }

    QString csv;
    csv += "number,type,pre-number,date,user,cleanup,description,userdata\n";

    const snapper::Snapshots &snapshots = snapper->getSnapshots();
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
        const snapper::Snapshot &snapshot = *it;

        csv += QString::number(snapshot.getNum()) + ",";
        csv += snapshotTypeToString(snapshot.getType()) + ",";
        csv += QString::number(snapshot.getPreNum()) + ",";

        // 日時をISO形式に変換
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(snapshot.getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(snapshot.getUid()) + ",";
        csv += QString::fromStdString(snapshot.getCleanup()) + ",";
        csv += QString::fromStdString(snapshot.getDescription()) + ",";

        // ユーザデータを key1=value1,key2=value2形式に変換
        const std::map<std::string, std::string> &userdata = snapshot.getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                               QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");
        csv += "\n";
    }

    return csv;
}

/**
 * @brief 利用可能なSnapper設定名のリストを返す
 *
 * libsnapperのgetConfigs()を呼び出し、存在する全Snapper設定 (例: "root", "home") の設定名を抽出して配列で返す。
 * スナップショット本体は返さない。
 * PolicyKit認証 (list-snapshots) を必要とする。
 *
 * @return 設定名の配列、失敗時は空配列
 */
QStringList SnapshotOperations::ListConfigs()
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QStringList();
    }

    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        QStringList configs;
        for (const auto &ci : configList) {
#if LIBSNAPPER_VERSION_AT_LEAST(6, 0)
            configs.append(QString::fromStdString(ci.get_config_name()));
#else
            configs.append(QString::fromStdString(ci.getConfigName()));
#endif
        }
        return configs;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapper configs:" << e.what();
        return QStringList();
    }
}

/**
 * @brief 指定設定のスナップショット一覧をCSVで取得する
 *
 * 空文字列のconfigNameは"root"と解釈される。
 * 呼び出し毎にSnapperインスタンスを強制再構築し、外部 (snapperd/snapper CLI等) で作成された新規スナップショットを確実に反映する。
 * PolicyKit認証 (list-snapshots) を必要とする。
 *
 * @param configName Snapper設定名 (空文字列時は"root")
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::ListSnapshots(const QString &configName)
{
    // resolveConfigOrFail が空文字列を "root" に正規化した上で検証する。
    // 失敗時はD-Busエラー応答が送出済み。
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        // 一覧取得時は必ず再構築して外部で作成された最新スナップショットを反映する
        snapper::Snapper *snapper = getSnapper(*cfg, /*forceReload=*/true);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        return formatSnapshotToCSV(snapper);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to list snapshots: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 新しいスナップショットを作成
 *
 * 指定されたパラメータで新しいスナップショットを作成します。
 * single、pre、postの3種類のタイプをサポートします。
 *
 * @param type スナップショットのタイプ ("single", "pre", "post")
 * @param description スナップショットの説明
 * @param preNumber postタイプの場合の対応するpreスナップショット番号
 * @param cleanup クリーンアップアルゴリズム名
 * @param important 重要フラグ
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::CreateSnapshot(const QString &configName, const QString &type, const QString &description,
                                           int preNumber, const QString &cleanup,
                                           const QMap<QString, QString> &userdata, bool important)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }
    if (!checkAuthorization("com.presire.qsnapper.create-snapshot")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::SCD scd;
        scd.description = description.toStdString();
        scd.cleanup = cleanup.toStdString();
        scd.read_only = true;

        // ユーザが指定した key=value 形式のユーザデータをコピー
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            scd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

        if (important) {
            scd.userdata["important"] = "yes";
        }

        snapper::Snapshots::iterator newSnapshot;
        snapper::SnapshotType snapType = static_cast<snapper::SnapshotType>(stringToSnapshotType(type));

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif
        if (snapType == snapper::PRE) {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPreSnapshot(scd, report);
#else
            newSnapshot = snapper->createPreSnapshot(scd);
#endif
        }
        else if (snapType == snapper::POST && preNumber > 0) {
            snapper::Snapshots::const_iterator preSnap = snapper->getSnapshots().find(preNumber);
            if (preSnap == snapper->getSnapshots().end()) {
                sendErrorReply(QDBusError::Failed, "Pre-snapshot not found");
                return QString();
            }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPostSnapshot(preSnap, scd, report);
#else
            newSnapshot = snapper->createPostSnapshot(preSnap, scd);
#endif
        }
        else {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createSingleSnapshot(scd, report);
#else
            newSnapshot = snapper->createSingleSnapshot(scd);
#endif
        }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        logPluginReport(report);
#endif

        // 新しく作成されたスナップショットのCSV情報を返す
        QString csv = "number,type,pre-number,date,user,cleanup,description,userdata\n";
        csv += QString::number(newSnapshot->getNum()) + ",";
        csv += snapshotTypeToString(newSnapshot->getType()) + ",";
        csv += QString::number(newSnapshot->getPreNum()) + ",";

        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(newSnapshot->getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(newSnapshot->getUid()) + ",";
        csv += QString::fromStdString(newSnapshot->getCleanup()) + ",";
        csv += QString::fromStdString(newSnapshot->getDescription()) + ",";

        const std::map<std::string, std::string> &userdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                                 QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");

        return csv;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to create snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to create snapshot: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 既存スナップショットのメタデータを編集
 *
 * description / cleanup algorithm / userdata を差し替えます。
 * 空文字列("")のdescriptionはそのまま空文字列で上書きされます。
 * userdataは渡されたマップで完全に置き換わります。(差分ではない)
 * PolicyKit認証を必要とします。
 *
 * @param configName Snapper設定名
 * @param number 編集対象のスナップショット番号
 * @param description 新しい説明文 (空文字列も可)
 * @param cleanup 新しいcleanupアルゴリズム名
 * @param userdata 新しいuserdataマップ (置換)
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::ModifySnapshot(const QString &configName, int number,
                                        const QString &description, const QString &cleanup,
                                        const QMap<QString, QString> &userdata)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    if (!checkAuthorization("com.presire.qsnapper.modify-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        snapper::SMD smd;
        smd.description = description.toStdString();
        smd.cleanup     = cleanup.toStdString();
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            smd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->modifySnapshot(snapshot, smd, report);
        logPluginReport(report);
#else
        snapper->modifySnapshot(snapshot, smd);
#endif
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to modify snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to modify snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief スナップショットを削除する (D-Busスロット)
 *
 * Polkit認証は毎回 checkAuthorization()に委ねる。
 * 連続削除時の再入力はpolkitのauth_admin_keep設定により、short-lived cookieで抑止される。
 *
 * @param configName 設定名
 * @param number 削除対象スナップショット番号
 * @return 成功時true
 */
bool SnapshotOperations::DeleteSnapshot(const QString &configName, int number)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    if (!checkAuthorization("com.presire.qsnapper.delete-snapshot")) {
        return false;
    }

    resetIdleTimer();

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->deleteSnapshot(snapshot, report);
        logPluginReport(report);
#else
        snapper->deleteSnapshot(snapshot);
#endif
        resetIdleTimer();   // 長時間削除後もタイマリセット
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to delete snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        resetIdleTimer();   // 例外時もタイマリセット
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時にそのスナップショットの状態で起動するようにします。
 *
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時: true、失敗時: false
 */
bool SnapshotOperations::RollbackSnapshot(const QString &configName, int number)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots &snapshots = snapper->getSnapshots();
        snapper::Snapshots::iterator target = snapshots.find(number);
        if (target == snapshots.end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // "sudo snapper rollback N"と同等の挙動を再現する。
        //
        // CLI (client/snapper/cmd-rollback.cc) はambitを以下で判定する:
        //   - previous_defaultがread-only --> TRANSACTIONAL
        //     (新規スナップショット作成なしで対象を直接default化)
        //   - previous_defaultがwritable --> CLASSIC
        //       (1) 現在状態のread-onlyバックアップsnapshotを作成
        //       (2) 対象Nのwritable copy snapshotを作成
        //       (3) previous_defaultにcleanupが空なら"number"を付与
        //       (4) (2)で作成したwritable copyをdefaultに設定
        snapper::Snapshots::iterator previousDefault = snapshots.getDefault();
        const bool transactional =
            (previousDefault != snapshots.end() && previousDefault->isReadOnly());

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif

        if (transactional) {
            // TRANSACTIONAL: 対象スナップショットをそのままdefaultにする
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            target->setDefault(report);
            logPluginReport(report);
#else
            target->setDefault();
#endif
        }
        else {
            // CLASSIC: backup + writable copyを作成して、writable copyをdefaultにする

            const int prevNum =
                (previousDefault != snapshots.end()) ? static_cast<int>(previousDefault->getNum()) : -1;

            // (1) 現在状態のread-onlyバックアップ
            snapper::SCD scd1;
            scd1.description = (prevNum >= 0)
                ? std::string("rollback backup of #") + std::to_string(prevNum)
                : std::string("rollback backup");
            scd1.cleanup = "number";
            scd1.userdata["important"] = "yes";
            scd1.read_only = true;

            // (2) 対象Nのwritable copy
            snapper::SCD scd2;
            scd2.description = std::string("writable copy of #") + std::to_string(number);
            scd2.cleanup.clear();
            scd2.read_only = false;

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1, report);
            logPluginReport(report);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2, report);
            logPluginReport(report);

            // (3) previous_defaultにcleanupが空なら"number"を付与
            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd, report);
                logPluginReport(report);
            }

            // (4) writable copyをdefaultに
            writableCopy->setDefault(report);
            logPluginReport(report);
#else
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2);

            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd);
            }

            writableCopy->setDefault();
#endif
            (void)backup;
        }

        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to rollback snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to rollback snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief ファイル変更一覧を取得
 *
 * 指定されたスナップショットと現在のシステム状態を比較し、変更されたファイルの一覧を取得します。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::GetFileChanges(const QString &configName, int snapshotNumber)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }
    if (!checkAuthorization("com.presire.qsnapper.view-diff")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // snapshot1: 比較元 (指定されたスナップショット)
        // snapshot2: 比較先 (現在のシステム状態)
        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを作成してファイル変更を取得
        // snapshot1からsnapshot2への変更を取得
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, false);
        const snapper::Files &files = comparison.getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

            // ステータスフラグを文字列に変換
            QString statusStr;
            if (status & snapper::CREATED) statusStr += "+";
            if (status & snapper::DELETED) statusStr += "-";
            if (status & snapper::TYPE) statusStr += "t";
            if (status & snapper::CONTENT) statusStr += "c";
            if (status & snapper::PERMISSIONS) statusStr += "p";
            if (status & snapper::OWNER) statusStr += "u";
            if (status & snapper::GROUP) statusStr += "g";
            if (status & snapper::XATTRS) statusStr += "x";
            if (status & snapper::ACL) statusStr += "a";

            if (statusStr.isEmpty()) statusStr = ".....";

            // パディングして出力フォーマットを整える
            statusStr = statusStr.leftJustified(5, '.');

            output += statusStr + " " + QString::fromStdString(file.getName()) + "\n";
        }

        return output;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}



/**
 * @brief 2 つのスナップショット間のファイル変更リストを取得
 *
 * snapshot1 → snapshot2の差分を取得します。現在のシステム状態は使用しません。
 * GetFileChangesの「任意の2つのsnapshot間」版です。
 */
QString SnapshotOperations::GetFileChangesBetween(const QString &configName, int number1, int number2)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }
    if (!checkAuthorization("com.presire.qsnapper.view-diff")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        snapper::Comparison comparison(snapper, snapshot1, snapshot2, false);
        const snapper::Files &files = comparison.getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

            QString statusStr;
            if (status & snapper::CREATED) statusStr += "+";
            if (status & snapper::DELETED) statusStr += "-";
            if (status & snapper::TYPE) statusStr += "t";
            if (status & snapper::CONTENT) statusStr += "c";
            if (status & snapper::PERMISSIONS) statusStr += "p";
            if (status & snapper::OWNER) statusStr += "u";
            if (status & snapper::GROUP) statusStr += "g";
            if (status & snapper::XATTRS) statusStr += "x";
            if (status & snapper::ACL) statusStr += "a";
            if (statusStr.isEmpty()) statusStr = ".....";
            statusStr = statusStr.leftJustified(5, '.');

            output += statusStr + " " + QString::fromStdString(file.getName()) + "\n";
        }

        return output;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes between snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間の個別ファイルの詳細 + diffを取得
 *
 * GetFileDiffAndDetailsの任意2つのsnapshot間版
 * snapshot1側のパーミッションとsnapshot2側のパーミッションを返し、diff部も両snapshot上のファイルを比較します。
 */
QString SnapshotOperations::GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!checkAuthorization("com.presire.qsnapper.view-diff")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        const snapper::Files &files = comparison.getFiles();

        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        unsigned int status = fileIt->getPreToPostStatus();
        QString statusStr;
        if (status & snapper::CREATED) statusStr += "+";
        if (status & snapper::DELETED) statusStr += "-";
        if (status & snapper::TYPE) statusStr += "t";
        if (status & snapper::CONTENT) statusStr += "c";
        if (status & snapper::PERMISSIONS) statusStr += "p";
        if (status & snapper::OWNER) statusStr += "u";
        if (status & snapper::GROUP) statusStr += "g";
        if (status & snapper::XATTRS) statusStr += "x";
        if (status & snapper::ACL) statusStr += "a";
        if (statusStr.isEmpty()) statusStr = ".....";
        statusStr = statusStr.leftJustified(5, '.');

        auto permsToOctal = [](QFile::Permissions p) -> QString {
            int mode = 0;
            if (p & QFile::ReadOwner)  mode |= 0400;
            if (p & QFile::WriteOwner) mode |= 0200;
            if (p & QFile::ExeOwner)   mode |= 0100;
            if (p & QFile::ReadGroup)  mode |= 0040;
            if (p & QFile::WriteGroup) mode |= 0020;
            if (p & QFile::ExeGroup)   mode |= 0010;
            if (p & QFile::ReadOther)  mode |= 0004;
            if (p & QFile::WriteOther) mode |= 0002;
            if (p & QFile::ExeOther)   mode |= 0001;
            return QString("%1").arg(mode, 4, 8, QChar('0'));
        };

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        // snapshot1をLOC_PREとして扱い、snapshot2をLOC_POSTとして扱う
        QString path1 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        QFileInfo info1(path1);
        if (info1.exists()) {
            detailsPart += "snapshotPerms=" + permsToOctal(info1.permissions()) + "\n";
            detailsPart += "snapshotOwner=" + info1.owner() + "\n";
            detailsPart += "snapshotGroup=" + info1.group() + "\n";
        }

        QString path2 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_POST));
        QFileInfo info2(path2);
        if (info2.exists()) {
            detailsPart += "currentPerms=" + permsToOctal(info2.permissions()) + "\n";
            detailsPart += "currentOwner=" + info2.owner() + "\n";
            detailsPart += "currentGroup=" + info2.group() + "\n";
        }

        QString diffPart;
        if (info1.exists() && info2.exists()) {
            diffPart = generateUnifiedDiff(path1, path2);
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff between snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルの差分と詳細情報を一括取得
 *
 * 1回のComparisonオブジェクト生成で、差分(diff)と詳細情報(パーミッション等)の両方を取得します。
 * GetFileDiff + GetFileDetailsの統合版。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 対象ファイルパス
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::GetFileDiffAndDetails(const QString &configName, int snapshotNumber, const QString &filePath)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!checkAuthorization("com.presire.qsnapper.view-diff")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを1回だけ作成 (スナップショットマウントも1回のみ) 
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        const snapper::Files &files = comparison.getFiles();

        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        // --- Details部の構築 ---
        unsigned int status = fileIt->getPreToPostStatus();
        QString statusStr;
        if (status & snapper::CREATED) statusStr += "+";
        if (status & snapper::DELETED) statusStr += "-";
        if (status & snapper::TYPE) statusStr += "t";
        if (status & snapper::CONTENT) statusStr += "c";
        if (status & snapper::PERMISSIONS) statusStr += "p";
        if (status & snapper::OWNER) statusStr += "u";
        if (status & snapper::GROUP) statusStr += "g";
        if (status & snapper::XATTRS) statusStr += "x";
        if (status & snapper::ACL) statusStr += "a";
        if (statusStr.isEmpty()) statusStr = ".....";
        statusStr = statusStr.leftJustified(5, '.');

        auto permsToOctal = [](QFile::Permissions p) -> QString {
            int mode = 0;
            if (p & QFile::ReadOwner)  mode |= 0400;
            if (p & QFile::WriteOwner) mode |= 0200;
            if (p & QFile::ExeOwner)   mode |= 0100;
            if (p & QFile::ReadGroup)  mode |= 0040;
            if (p & QFile::WriteGroup) mode |= 0020;
            if (p & QFile::ExeGroup)   mode |= 0010;
            if (p & QFile::ReadOther)  mode |= 0004;
            if (p & QFile::WriteOther) mode |= 0002;
            if (p & QFile::ExeOther)   mode |= 0001;
            return QString("%1").arg(mode, 4, 8, QChar('0'));
        };

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        QString snapshotPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        QFileInfo snapshotInfo(snapshotPath);
        if (snapshotInfo.exists()) {
            detailsPart += "snapshotPerms=" + permsToOctal(snapshotInfo.permissions()) + "\n";
            detailsPart += "snapshotOwner=" + snapshotInfo.owner() + "\n";
            detailsPart += "snapshotGroup=" + snapshotInfo.group() + "\n";
        }

        QString currentPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_SYSTEM));
        QFileInfo currentInfo(currentPath);
        if (currentInfo.exists()) {
            detailsPart += "currentPerms=" + permsToOctal(currentInfo.permissions()) + "\n";
            detailsPart += "currentOwner=" + currentInfo.owner() + "\n";
            detailsPart += "currentGroup=" + currentInfo.group() + "\n";
        }

        // --- Diff部の取得 ---
        QString diffPart;
        if (snapshotInfo.exists() && currentInfo.exists()) {
            diffPart = generateUnifiedDiff(snapshotPath, currentPath);
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff and details:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file diff and details: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルをスナップショットから復元する (YaST 互換経路)
 *
 * 内部で restoreFilesImpl を呼び出すだけのラッパ。
 * reflink は使用せず typechanged の事前削除も行わない。
 *
 * @param configName      Snapper設定名
 * @param snapshotNumber  復元元スナップショット番号
 * @param filePaths       復元対象絶対パス
 * @param changeTypes     各ファイルの変更種別
 * @return 全ファイル成功時 true
 */
bool SnapshotOperations::RestoreFiles(const QString &configName, int snapshotNumber,
                                      const QStringList &filePaths, const QStringList &changeTypes)
{
    return restoreFilesImpl(configName, snapshotNumber, filePaths, changeTypes,
                            /*useReflink=*/false,
                            /*removeOnTypechanged=*/false,
                            "RestoreFiles");
}

/**
 * @brief ファイルをスナップショットから復元する (高速経路)
 *
 * 内部でrestoreFilesImplを呼び出すだけのラッパー
 * btrfs reflink (FICLONE) を優先し、typechanged時は既存ファイルを削除してから上書きする。
 *
 * @param configName      Snapper設定名
 * @param snapshotNumber  復元元スナップショット番号
 * @param filePaths       復元対象絶対パス
 * @param changeTypes     各ファイルの変更種別
 * @return 全ファイル成功時 true
 */
bool SnapshotOperations::RestoreFilesDirect(const QString &configName, int snapshotNumber,
                                            const QStringList &filePaths, const QStringList &changeTypes)
{
    return restoreFilesImpl(configName, snapshotNumber, filePaths, changeTypes,
                            /*useReflink=*/true,
                            /*removeOnTypechanged=*/true,
                            "RestoreFilesDirect");
}

/**
 * @brief RestoreFiles / RestoreFilesDirect共通実装
 *
 * 主な差分:
 *   - useReflink:          通常ファイルコピー時にFICLONE (btrfs CoW)を試行するか
 *   - removeOnTypechanged: typechanged時に既存ファイルを先にrmするか (ディレクトリ --> ファイル変化対策)
 *
 * セキュリティ要件:
 *   - configNameはresolveConfigOrFailで検証済みであること (呼び出し側の責務でないため本関数でも検証)
 *   - filePathsの各要素は絶対パスで、かつ snapshotDir配下を指すこと
 *     (snapshotFilePath = snapshotDir + filePathがsnapshotDir内に収まることをisPathWithinSnapshotRootで検証)
 *   - "/.snapshots/" 直下への書き込み (systemFilePath側) は書き込み対象として棄却する
 *   - シンボリックリンク解決は copySymlink / copyRegularFileのレイヤーで行う (本関数はパスの構文検証のみ)
 */
bool SnapshotOperations::restoreFilesImpl(const QString &configName, int snapshotNumber,
                                          const QStringList &filePaths,
                                          const QStringList &changeTypes,
                                          bool useReflink, bool removeOnTypechanged,
                                          const char *logTag)
{
    resetIdleTimer();

    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return false;
    }

    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    if (filePaths.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, "No files specified for restore");
        return false;
    }

    if (filePaths.size() != changeTypes.size()) {
        sendErrorReply(QDBusError::InvalidArgs, "filePaths and changeTypes must have the same size");
        return false;
    }

    qInfo() << logTag << ": Starting restore for" << filePaths.size()
            << "files from snapshot" << snapshotNumber
            << "(useReflink=" << useReflink
            << ", removeOnTypechanged=" << removeOnTypechanged << ")";

    try {
        snapper::Snapper *snapper = getSnapper(*cfg);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショットをマウント
        snapshot1->mountFilesystemSnapshot(true);

        // スナップショットディレクトリのパスを取得
        QString snapshotDir = QString::fromStdString(snapshot1->snapshotDir());

        qInfo() << logTag << ": Snapshot mounted at" << snapshotDir;

        bool allSuccess = true;
        int total = filePaths.size();
        int successCount = 0;
        int skippedCount = 0;

        for (int i = 0; i < total; ++i) {
            const QString &filePath = filePaths[i];
            const QString &changeType = changeTypes[i];

            // 入力検証 (進捗 emit より前に行い、未検証パスをD-Busシグナルへ漏出させない)
            // (1) 絶対パスでなければ拒否
            if (!filePath.startsWith(QLatin1Char('/'))) {
                qWarning() << logTag << ": Rejecting non-absolute path:" << filePath;
                skippedCount++;
                continue;
            }

            // (2) 書き込み先として /.snapshots/ 直下は禁止 (スナップショット木の破壊防止)
            if (filePath.startsWith(QStringLiteral("/.snapshots/"))) {
                qWarning() << logTag << ": Skipping dangerous destination path:" << filePath;
                skippedCount++;
                continue;
            }

            // (3) snapshotDir + filePathがsnapshotDir配下に収まっていること
            //     (".."を含むfilePathによるsnapshotツリー外参照を防ぐ)
            const QString snapshotFilePath = snapshotDir + filePath;
            if (!qsnapper::security::isPathWithinSnapshotRoot(snapshotFilePath, snapshotDir)) {
                qWarning() << logTag << ": Rejecting path escaping snapshot root:" << filePath;
                skippedCount++;
                continue;
            }

            // 検証通過後にのみ進捗を通知 (D-Busシグナルが運ぶのは受理済みパスのみ)
            emit restoreProgress(i + 1, total, filePath);

            // システム上のファイルパス (ルートからの絶対パス)
            const QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル --> 削除
                std::error_code ec;
                std::filesystem::remove_all(systemFilePath.toStdString(), ec);
                fileSuccess = !ec;
                if (!fileSuccess) {
                    qWarning() << logTag << ": Failed to remove" << systemFilePath
                               << ec.message().c_str();
                }
            }
            else {
                // deleted / modified / typechanged --> スナップショットからコピー

                // 親ディレクトリを確認・作成
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parentDir.toStdString(), ec);
                }

                QFileInfo snapshotFileInfo(snapshotFilePath);
                if (snapshotFileInfo.isSymLink()) {
                    // シンボリックリンクの場合
                    fileSuccess = copySymlink(snapshotFilePath, systemFilePath);
                    if (!fileSuccess) {
                        qWarning() << logTag << ": Failed to copy symlink" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else if (snapshotFileInfo.isDir()) {
                    // ディレクトリの場合: 作成 + chown + chmod
                    if (!QFileInfo::exists(systemFilePath)) {
                        std::error_code ec;
                        std::filesystem::create_directories(systemFilePath.toStdString(), ec);
                    }

                    // 所有者をコピー
                    chown(systemFilePath.toUtf8().constData(),
                          snapshotFileInfo.ownerId(), snapshotFileInfo.groupId());

                    // パーミッションをコピー (POSIX stat + chmod)
                    struct stat st;
                    if (lstat(snapshotFilePath.toUtf8().constData(), &st) == 0) {
                        chmod(systemFilePath.toUtf8().constData(), st.st_mode);
                    }

                    fileSuccess = true;
                }
                else if (snapshotFileInfo.exists()) {
                    // typechanged の事前削除 (Direct経路のみ有効)
                    if (removeOnTypechanged && changeType == "typechanged"
                            && QFileInfo::exists(systemFilePath)) {
                        std::error_code ec;
                        std::filesystem::remove_all(systemFilePath.toStdString(), ec);
                        if (ec) {
                            qWarning() << logTag << ": Failed to remove before copy"
                                       << systemFilePath << ec.message().c_str();
                            allSuccess = false;
                            continue;
                        }
                    }

                    // 通常ファイルの場合 (useReflink=trueならFICLONEを先行試行)
                    fileSuccess = copyRegularFile(snapshotFilePath, systemFilePath, useReflink);
                    if (!fileSuccess) {
                        qWarning() << logTag << ": Failed to copy" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else {
                    qWarning() << logTag << ": Source not found in snapshot:" << snapshotFilePath;
                    fileSuccess = false;
                }
            }

            if (fileSuccess) {
                successCount++;
            }
            else {
                allSuccess = false;
            }
        }

        // 安全ネット: 復元操作によりルートサブボリュームがread-onlyになっていないか確認・復旧
        {
            bool isReadOnly = false;
            if (btrfs_util_get_subvolume_read_only("/", &isReadOnly) == BTRFS_UTIL_OK && isReadOnly) {
                qWarning() << logTag << ": Root subvolume became read-only after restore, restoring rw";
                btrfs_util_set_subvolume_read_only("/", false);
            }
        }

        // スナップショットをアンマウント
        try {
            snapshot1->umountFilesystemSnapshot(true);
        }
        catch (...) {
            qWarning() << logTag << ": Failed to unmount snapshot";
        }

        if (skippedCount > 0) {
            qWarning() << logTag << ": Skipped" << skippedCount << "dangerous paths";
        }
        qInfo() << logTag << ": Completed. Successful:" << successCount
                << "Failed:" << (total - successCount - skippedCount);

        if (!allSuccess) {
            QString errorMsg = QString("Failed to restore %1 out of %2 files")
                    .arg(total - successCount).arg(total);
            sendErrorReply(QDBusError::Failed, errorMsg);
        }

        return allSuccess;
    }
    catch (const snapper::Exception &e) {
        qWarning() << logTag << " failed:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to restore files: %1").arg(e.what()));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << logTag << " unexpected error:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Unexpected error: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief 通常ファイルをコピー (sendfile + 権限・所有者・タイムスタンプ保持)
 *
 * tryReflink=trueの場合、まずioctl(FICLONE)を試行し、
 * btrfs CoW (reflink)が使用可能であれば高速コピー、失敗時はsendfileにフォールバック。
 *
 * "cp -d --preserve=all --no-preserve=xattr"と同等の動作。
 */
bool SnapshotOperations::copyRegularFile(const QString &src, const QString &dst, bool tryReflink)
{
    int srcFd = open(src.toUtf8().constData(), O_RDONLY);
    if (srcFd < 0) {
        qWarning() << "copyRegularFile: Failed to open source:" << src << strerror(errno);
        return false;
    }

    struct stat srcStat;
    if (fstat(srcFd, &srcStat) < 0) {
        qWarning() << "copyRegularFile: Failed to stat source:" << src << strerror(errno);
        close(srcFd);
        return false;
    }

    int dstFd = open(dst.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, srcStat.st_mode);
    if (dstFd < 0) {
        qWarning() << "copyRegularFile: Failed to open destination:" << dst << strerror(errno);
        close(srcFd);
        return false;
    }

    bool copied = false;

    // Step 1: reflink (btrfs CoW)を試行
    if (tryReflink) {
        if (ioctl(dstFd, FICLONE, srcFd) == 0) {
            copied = true;
        }
        // FICLONE 失敗時は sendfile にフォールバック
    }

    // Step 2: sendfileでデータコピー
    if (!copied) {
        off_t offset = 0;
        ssize_t remaining = srcStat.st_size;
        while (remaining > 0) {
            ssize_t written = sendfile(dstFd, srcFd, &offset, remaining);
            if (written < 0) {
                qWarning() << "copyRegularFile: sendfile failed:" << strerror(errno);
                close(dstFd);
                close(srcFd);
                return false;
            }
            remaining -= written;
        }
    }

    // 所有者を保持 (cp --preserve=all)
    if (fchown(dstFd, srcStat.st_uid, srcStat.st_gid) < 0) {
        // root権限でのみ成功する; 失敗は警告のみ
        qWarning() << "copyRegularFile: fchown failed (non-fatal):" << strerror(errno);
    }

    // タイムスタンプを保持
    struct timespec ts[2];
    ts[0] = srcStat.st_atim;
    ts[1] = srcStat.st_mtim;
    futimens(dstFd, ts);

    close(dstFd);
    close(srcFd);
    return true;
}

/**
 * @brief シンボリックリンクをコピー (readlink → symlink + lchown + タイムスタンプ)
 *
 * "cp -d --preserve=all --no-preserve=xattr"のシンボリックリンク版。
 */
bool SnapshotOperations::copySymlink(const QString &src, const QString &dst)
{
    char buf[PATH_MAX];
    ssize_t len = readlink(src.toUtf8().constData(), buf, sizeof(buf) - 1);
    if (len < 0) {
        qWarning() << "copySymlink: readlink failed:" << src << strerror(errno);
        return false;
    }
    buf[len] = '\0';

    // 既存ファイル/リンクを先に削除
    std::filesystem::remove(std::filesystem::path(dst.toStdString()));

    if (symlink(buf, dst.toUtf8().constData()) < 0) {
        qWarning() << "copySymlink: symlink failed:" << dst << strerror(errno);
        return false;
    }

    // 所有者を保持 (lchown = リンク自体の所有者を変更)
    struct stat srcStat;
    if (lstat(src.toUtf8().constData(), &srcStat) == 0) {
        lchown(dst.toUtf8().constData(), srcStat.st_uid, srcStat.st_gid);
    }

    // タイムスタンプを保持 (l utimes 相当)
    struct timespec ts[2];
    ts[0] = srcStat.st_atim;
    ts[1] = srcStat.st_mtim;
    utimensat(AT_FDCWD, dst.toUtf8().constData(), ts, AT_SYMLINK_NOFOLLOW);

    return true;
}
