#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QPointer>
// glibのGDBusInterfaceInfo / GDBusObjectSkeletonは "signals" というメンバ名を持ち、
// Qtの signals マクロと衝突する。polkitヘッダの取り込み中だけマクロを外す
#undef signals
#include <polkit/polkit.h>
#define signals Q_SIGNALS
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <snapper/Version.h>
#include <btrfsutil.h>
#include <algorithm>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utime.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <limits.h>
#include <sys/xattr.h>
#include "snapshotoperations.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"

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
// Polkit非同期認可
//
// polkit-qt6-1の非同期API (Authority::checkAuthorization + checkAuthorizationFinished)
// は使用しない。Authorityはシングルトンであり、完了シグナルはResult値しか運ばず、
// 完了callbackのuser_dataもAuthority自身であるため、同時に2件の認可が進行すると
// どのD-Bus呼び出しに対する結果なのかを判別できない (rootサービスでは
// 「別要求の許可」を流用してしまう認可の取り違えに直結する)。
// またcancellableとエラー状態がシングルトンで共有されており、1件の失敗が
// 後続の全呼び出しを黙って落とす。
//
// polkitのGObject APIは呼び出しごとにGSimpleAsyncResultとuser_dataを確保するため、
// 完了callbackを発行元の呼び出しへ厳密に対応付けられる。完了callbackは呼び出し時点の
// thread-default GMainContext (=Qtのイベントループが回すdefault context) で発火する
// ============================================================================

namespace {

    /**
     * @brief polkit authorityをプロセス寿命の間だけ取得して再利用する
     * @return 取得済みauthority、取得できなければnullptr
     */
    PolkitAuthority *polkitAuthorityInstance()
    {
        static PolkitAuthority *authority = []() -> PolkitAuthority * {
            GError *error = nullptr;
            PolkitAuthority *result = polkit_authority_get_sync(nullptr, &error);
            if (!result) {
                qCritical() << "Failed to obtain polkit authority:"
                            << (error ? error->message : "unknown error");
            }
            if (error) {
                g_error_free(error);
            }
            return result;
        }();
        return authority;
    }

    /**
     * @brief 非同期認可1件分の状態 (polkitのuser_dataとして渡す)
     */
    struct AsyncAuthorization {
        // サービスが先に破棄された場合に継続を実行しないためのguard
        QPointer<SnapshotOperations> service;
        std::function<void(bool)> continuation;
    };

    /**
     * @brief polkit非同期認可の完了callback
     *
     * user_dataは本呼び出し専用に確保したAsyncAuthorizationであり、
     * 他の認可要求の結果と混ざらない
     *
     * @param source 認可を行ったPolkitAuthority
     * @param result 完了結果
     * @param userData AsyncAuthorization* (本callbackが所有権を引き取る)
     */
    void asyncAuthorizationFinished(GObject *source,
                                    GAsyncResult *result,
                                    gpointer userData)
    {
        const std::unique_ptr<AsyncAuthorization> request(
            static_cast<AsyncAuthorization *>(userData));

        GError *error = nullptr;
        PolkitAuthorizationResult *authorization =
            polkit_authority_check_authorization_finish(
                POLKIT_AUTHORITY(source), result, &error);

        bool granted = false;
        if (authorization) {
            granted = polkit_authorization_result_get_is_authorized(authorization);
            g_object_unref(authorization);
        }
        else {
            qWarning() << "Polkit authorization check failed:"
                       << (error ? error->message : "unknown error");
        }
        if (error) {
            g_error_free(error);
        }

        // サービスが既に破棄されている場合は応答先も存在しない
        if (request->service.isNull() || !request->continuation) {
            return;
        }
        request->continuation(granted);
    }

}

// ============================================================================
// In-process unified diff (Myers diff algorithm)
// "diff -u"コマンドの置き換え
// ============================================================================

namespace {

    // 新規inodeへ差し替える復元経路で、snapshot側のSELinux labelを引き継ぐ。
    // SELinux無効環境やlabel非対応FSではENODATA / ENOTSUPになるため、成否は非致命として扱う
    void copySecurityContextBestEffort(int sourceFd, int destinationFd)
    {
        static constexpr const char *kSelinuxXattr = "security.selinux";

        const ssize_t length = ::fgetxattr(sourceFd, kSelinuxXattr, nullptr, 0);
        if (length <= 0) {
            return;
        }

        QByteArray context(static_cast<qsizetype>(length), '\0');
        const ssize_t read = ::fgetxattr(sourceFd, kSelinuxXattr, context.data(),
                                         static_cast<size_t>(context.size()));
        if (read <= 0) {
            return;
        }

        context.truncate(static_cast<qsizetype>(read));
        if (::fsetxattr(destinationFd, kSelinuxXattr, context.constData(),
                        static_cast<size_t>(context.size()), 0) < 0) {
            qWarning() << "copySecurityContextBestEffort: fsetxattr failed (non-fatal):"
                       << strerror(errno);
        }
    }

    QString siblingTemporaryPath(const QString &path, const QString &tag, int attempt)
    {
        const int slashIndex = path.lastIndexOf(QLatin1Char('/'));
        const QString dirPath = slashIndex <= 0 ? QStringLiteral("/") : path.left(slashIndex);
        const QString baseName = slashIndex < 0 ? path : path.mid(slashIndex + 1);

        // suffixはASCIIのみで構成されるため、文字数がそのままbyte数になる
        const QString suffix = QStringLiteral(".") + tag
                + QStringLiteral(".") + QString::number(QCoreApplication::applicationPid())
                + QStringLiteral(".") + QString::number(QDateTime::currentMSecsSinceEpoch())
                + QStringLiteral(".") + QString::number(attempt);

        // leaf名はNAME_MAX (bytes) を超えられない。長いbase名はここで切り詰めるが、
        // suffixのpid / ms / attemptにより一意性は保たれる。
        // UTF-8の途中で切らないよう、収まるまで文字単位で削る
        const qsizetype maxBaseBytes = static_cast<qsizetype>(NAME_MAX) - 1 - suffix.size();
        QString trimmedBase = baseName;
        while (!trimmedBase.isEmpty() && trimmedBase.toUtf8().size() > maxBaseBytes) {
            trimmedBase.chop(1);
        }

        return dirPath
                + QLatin1Char('/')
                + QLatin1Char('.')
                + trimmedBase
                + suffix;
    }

    // live path 上の退避 rename も parent dirfd を固定した renameat() に寄せる。
    // これにより intermediate parent の symlink 差し替えに依存しない。
    bool movePathAsideNoFollow(const QString &path, QString *movedPath)
    {
        if (movedPath) {
            movedPath->clear();
        }

        for (int attempt = 0; attempt < 16; ++attempt) {
            const QString candidate = siblingTemporaryPath(path, QStringLiteral("qsnapper-old"), attempt);

            if (qsnapper::security::safeRenamePathNoFollow(path, candidate)) {
                if (movedPath) {
                    *movedPath = candidate;
                }
                return true;
            }

            if (errno == ENOENT) {
                return true;
            }

            if (errno == EEXIST || errno == ENOTEMPTY) {
                continue;
            }

            return false;
        }

        errno = EEXIST;
        return false;
    }

    QString ownerName(uid_t uid)
    {
        if (passwd *pwd = ::getpwuid(uid)) {
            return QString::fromLocal8Bit(pwd->pw_name);
        }
        return QString::number(uid);
    }

    QString groupName(gid_t gid)
    {
        if (group *grp = ::getgrgid(gid)) {
            return QString::fromLocal8Bit(grp->gr_name);
        }
        return QString::number(gid);
    }

    QString permsToOctal(mode_t mode)
    {
        return QString("%1").arg(static_cast<unsigned int>(mode & 07777), 4, 8, QChar('0'));
    }

    QString readTextFileNoFollow(const QString &path)
    {
        const int fd = qsnapper::security::safeOpenRegularFileRead(path);
        if (fd < 0) {
            return {};
        }

        QFile file;
        if (!file.open(fd, QIODevice::ReadOnly | QIODevice::Text, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return {};
        }

        return QString::fromUtf8(file.readAll());
    }

    struct DiffOp {
        enum Type { Equal, Delete, Insert };
        Type type;
        int aIdx, bIdx;  // 0-based index into old/new lines (-1 if N/A)
    };

    /**
    * Myers diffアルゴリズムで2つの文字列リスト間の最短編集スクリプトを計算する
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
    * 2つのファイルを読み込み、unified diff形式の文字列を生成する
    * "diff -u"コマンドと互換性のあるフォーマットで、QMLのformatDiffHtml()でパース可能
    *
    * @param oldPath 旧ファイルパス (--- ヘッダに使用)
    * @param newPath 新ファイルパス (+++ ヘッダに使用)
    * @return unified diff文字列、差分がない場合は空文字列
    */
    static QString generateUnifiedDiff(const QString &oldPath, const QString &newPath)
    {
        const QString oldContent = readTextFileNoFollow(oldPath);
        const QString newContent = readTextFileNoFollow(newPath);
        if (oldContent.isNull() || newContent.isNull())
            return {};

        QStringList a = oldContent.split('\n');
        QStringList b = newContent.split('\n');

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
 * スナップショット操作を管理するクラスを初期化する
 *
 * @param parent 親QObjectポインタ
 */
SnapshotOperations::SnapshotOperations(QObject *parent)
    : QObject(parent)
    , m_snapper(nullptr)
    , m_currentConfig("")
    , m_restoreExecutor(m_restoreRegistry)
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(IdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, []() {
        qInfo() << "Idle timeout reached, shutting down...";
        QCoreApplication::quit();
    });
    m_idleTimer.start();

    m_restoreExecutor.setEntryApplier(
        [this](const QString &manifestId,
               const qsnapper::restore::RestoreEntry &entry) {
            return applyRestoreEntry(manifestId, entry);
        });
    m_restoreExecutor.setProgressSink(
        [this](const QString &manifestId, int current, int total,
               const QString &path) {
            resetIdleTimer();
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

    m_ownerWatcher = new QDBusServiceWatcher(
        QString(), QDBusConnection::systemBus(),
        QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_ownerWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &SnapshotOperations::handleRestoreOwnerUnregistered);
}

/**
 * @brief SnapshotOperationsクラスのデストラクタ
 *
 * リソースのクリーンアップを行う
 */
SnapshotOperations::~SnapshotOperations()
{
    const QStringList planIds = m_restorePlanOwners.keys();
    for (const QString &manifestId : planIds) {
        m_restoreExecutor.abandon(manifestId);
    }

    const QStringList executionIds = m_restoreExecutions.keys();
    for (const QString &manifestId : executionIds) {
        cleanupRestoreExecution(manifestId);
    }
}

/**
 * @brief アイドルタイマをリセット
 *
 * D-Busメソッド呼び出し時にタイマをリセットし、アイドルタイムアウトを延長する
 *
 * polkitプロンプトの応答待ちが1件でも残っている間はタイマを止めたままにする
 * (プロンプトはタイムアウトを持たないため、認証中にアイドル終了してしまうと
 *  ユーザがパスワードを入力した直後に呼び出しが失われる)
 */
void SnapshotOperations::resetIdleTimer()
{
    if (m_pendingAuthorizations > 0) {
        m_idleTimer.stop();
        return;
    }
    m_idleTimer.start();
}

/**
 * @brief 現在のD-Bus呼び出し元unique nameを返す
 * @return D-Bus呼び出し時はmessage sender、それ以外は空文字列
 */
QString SnapshotOperations::callerOwner() const
{
    return calledFromDBus() ? message().service() : QString();
}

/**
 * @brief manifest操作エラーを情報漏洩しないD-Bus errorへ変換して送信する
 * @param error registryが返したエラー
 * @return 常にfalse
 */
bool SnapshotOperations::sendManifestError(
    qsnapper::restore::ManifestError error)
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

    replyError(errorType, messageText);
    return false;
}

/**
 * @brief manifest状態をD-Bus contractの小文字表現へ変換する
 * @param state 変換対象状態
 * @return contractで定義した状態文字列
 */
QString SnapshotOperations::restoreManifestStateString(
    qsnapper::restore::ManifestState state)
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
QString SnapshotOperations::restoreModeString(
    qsnapper::restore::RestoreMode mode)
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
QString SnapshotOperations::quoteRestoreStatusCsvField(const QString &field)
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
 * @brief execution contextに記録されたsnapshot mountを解除する
 * @param execution mount元設定とsnapshot番号を持つcontext
 */
void SnapshotOperations::unmountRestoreExecution(
    const RestoreExecution &execution)
{
    if (!execution.mounted) {
        return;
    }

    try {
        snapper::Snapper *snapper = getSnapper(execution.configName);
        if (!snapper) {
            qWarning() << "Staged restore: Failed to initialize Snapper for unmount";
            return;
        }

        const snapper::Snapshots::const_iterator snapshot =
            snapper->getSnapshots().find(execution.snapshotNumber);
        if (snapshot == snapper->getSnapshots().end()) {
            qWarning() << "Staged restore: Snapshot unavailable during unmount";
            return;
        }
        snapshot->umountFilesystemSnapshot(true);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Staged restore: Failed to unmount snapshot:" << e.what();
    }
    catch (const std::exception &e) {
        qWarning() << "Staged restore: Unexpected unmount failure:" << e.what();
    }
    catch (...) {
        qWarning() << "Staged restore: Unknown unmount failure";
    }
}

/**
 * @brief 指定manifestのmountを解除して実行contextを削除する
 * @param manifestId cleanup対象manifest id
 */
void SnapshotOperations::cleanupRestoreExecution(const QString &manifestId)
{
    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution == m_restoreExecutions.end()) {
        return;
    }

    const RestoreExecution context = execution.value();
    unmountRestoreExecution(context);
    if (execution->snapshotDirFd >= 0) {
        ::close(execution->snapshotDirFd);
        execution->snapshotDirFd = -1;
    }
    m_restoreExecutions.erase(execution);
}

/**
 * @brief 復元後にroot subvolumeがread-onlyならrwへ戻す安全ネットを実行する
 */
void SnapshotOperations::restoreRootReadWriteSafetyNet()
{
    bool isReadOnly = false;
    if (btrfs_util_get_subvolume_read_only("/", &isReadOnly) == BTRFS_UTIL_OK
            && isReadOnly) {
        qWarning() << "Staged restore: Root subvolume became read-only after restore, restoring rw";
        const auto result = btrfs_util_set_subvolume_read_only("/", false);
        if (result != BTRFS_UTIL_OK) {
            qCritical() << "Staged restore: Failed to restore root subvolume rw state:"
                        << result;
        }
    }
}

/**
 * @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
 * @param manifestId 終端したmanifest id
 * @param terminal 終端状態
 * @param messageText 終端理由
 */
void SnapshotOperations::finishRestorePlan(
    const QString &manifestId,
    qsnapper::restore::ManifestState terminal,
    const QString &messageText)
{
    restoreRootReadWriteSafetyNet();

    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution != m_restoreExecutions.end()) {
        unmountRestoreExecution(execution.value());
        if (execution->snapshotDirFd >= 0) {
            ::close(execution->snapshotDirFd);
            execution->snapshotDirFd = -1;
        }
    }

    emit restorePlanFinished(manifestId,
                             restoreManifestStateString(terminal),
                             messageText);

    m_restoreExecutions.remove(manifestId);
    m_restoreRegistry.remove(manifestId);
    m_restorePlanOwners.remove(manifestId);
    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief owner消失時に予約済み実行とmountとmanifestを全て破棄する
 * @param owner unregisterされたD-Bus unique name
 */
void SnapshotOperations::handleRestoreOwnerUnregistered(const QString &owner)
{
    QStringList ownedPlanIds;
    for (auto plan = m_restorePlanOwners.cbegin();
         plan != m_restorePlanOwners.cend(); ++plan) {
        if (plan.value() == owner) {
            ownedPlanIds.append(plan.key());
        }
    }

    for (const QString &manifestId : ownedPlanIds) {
        m_restoreExecutor.abandon(manifestId);
        cleanupRestoreExecution(manifestId);
        m_restorePlanOwners.remove(manifestId);
    }

    m_restoreRegistry.removeByOwner(owner);
    if (m_ownerWatcher && m_restoreRegistry.countForOwner(owner) == 0) {
        m_ownerWatcher->removeWatchedService(owner);
    }
}

/**
 * @brief TTL purgeで消えたactive計画をabandonしmountもcleanupする
 */
void SnapshotOperations::purgeExpiredRestorePlans()
{
    m_restoreRegistry.purgeExpired();

    const QStringList planIds = m_restorePlanOwners.keys();
    for (const QString &manifestId : planIds) {
        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const QString owner = m_restorePlanOwners.value(manifestId);
        if (!m_restoreRegistry.status(manifestId, owner, &error)) {
            // executorが同じ計画をもう1度終端しないよう先にabandonしてから、
            // finishRestorePlanで終端する。
            // ここでcleanupRestoreExecutionだけを呼ぶとrestorePlanFinishedが発火せず、
            // クライアントは完了通知を永久に待ち続け、復元でread-onlyになった
            // root subvolumeをrwへ戻す安全ネットも実行されない
            m_restoreExecutor.abandon(manifestId);
            finishRestorePlan(
                manifestId, qsnapper::restore::ManifestState::Failed,
                QStringLiteral("Restore plan expired before completion"));
        }
    }

    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief manifestを持たないownerをservice watcherから除外する
 */
void SnapshotOperations::removeUnusedRestoreOwnerWatches()
{
    if (!m_ownerWatcher) {
        return;
    }

    const QStringList watchedOwners = m_ownerWatcher->watchedServices();
    for (const QString &owner : watchedOwners) {
        if (m_restoreRegistry.countForOwner(owner) == 0) {
            m_ownerWatcher->removeWatchedService(owner);
        }
    }
}

/**
 * @brief Snapperが設定されているか確認
 *
 * Snapper設定が1つ以上存在するかを確認する
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
 * 指定されたキー/バリューペアをSnapper設定に書き込む
 * PolicyKit認証を必要とする
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

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.configure"),
        [this, config = *cfg, settings]() {
            return writeSnapperConfigAuthorized(config, settings);
        });
}

/**
 * @brief 認可済みのWriteSnapperConfig本体
 *
 * @param configName 検証済みSnapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::writeSnapperConfigAuthorized(
    const QString &configName, const QMap<QString, QString> &settings)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        // 設定変更前にComparisonキャッシュを無効化 (mount/Filesが古い設定状態に依存しないように)
        m_comparisonCache.clear();

        std::map<std::string, std::string> info;
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info[it.key().toStdString()] = it.value().toStdString();
        }

        snapper->setConfigInfo(info);
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to write snapper config:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to write config: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief Snapperのクォータを設定
 *
 * 指定されたSnapper設定のクォータ機能を設定する
 * PolicyKit認証を必要とする
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

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.configure"),
        [this, config = *cfg]() { return setupQuotaAuthorized(config); });
}

/**
 * @brief 認可済みのSetupQuota本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 成功時: true、失敗時: false
 */
bool SnapshotOperations::setupQuotaAuthorized(const QString &configName)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper->setupQuota();
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to setup quota:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to setup quota: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief configNameを正規化 + 検証し、不正ならD-Busエラー応答を送信する
 *
 * 空文字列入力を "root" に正規化した上で qsnapper::security::validateConfigName で検証する
 * 無効な場合はQDBusError::InvalidArgsを送信し、std::nulloptを返す
 * Polkitプロンプトを出す前に呼び出して、攻撃者が任意configNameでpolkitを浪費するのを防ぐ
 *
 * @param configName 検査する設定名 (空文字列は "root" として扱う)
 * @return 正規化後の設定名 (有効時)、無効でエラー送信済み (std::nullopt)
 */
std::optional<QString> SnapshotOperations::resolveConfigOrFail(const QString &configName)
{
    const QString effective = configName.isEmpty() ? QStringLiteral("root") : configName;
    if (!qsnapper::security::validateConfigName(effective)) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid configName"));
        return std::nullopt;
    }
    return effective;
}

/**
 * @brief 現在のD-Bus呼び出しの応答contextをcaptureする
 *
 * message()はスロットから戻ると無効になるため、認可待ちを跨ぐ経路では
 * 本関数の戻り値を保持して応答する
 *
 * @return capture済み応答context
 */
SnapshotOperations::CallReply SnapshotOperations::captureCallReply() const
{
    CallReply reply;
    if (!calledFromDBus()) {
        return reply;
    }

    reply.message = message();
    reply.connection = connection();
    reply.fromDBus = true;
    return reply;
}

/**
 * @brief 同期応答と遅延応答のどちらでも正しくD-Busエラーを返す
 *
 * @param type 返すD-Busエラー種別
 * @param text エラーメッセージ
 */
void SnapshotOperations::replyError(QDBusError::ErrorType type, const QString &text)
{
    if (m_deferredReply) {
        // 継続の中ではQDBusContextの呼び出しcontextが失われているため、
        // capture済みmessageから直接エラー応答を組み立てて送出する
        if (!m_deferredReply->replied && m_deferredReply->fromDBus) {
            m_deferredReply->replied = true;
            m_deferredReply->connection.send(
                m_deferredReply->message.createErrorReply(type, text));
        }
        return;
    }

    sendErrorReply(type, text);
}

/**
 * @brief 認可待ち1件の終了を記録し、必要ならアイドルタイマを再開する
 */
void SnapshotOperations::endPendingAuthorization()
{
    if (m_pendingAuthorizations > 0) {
        --m_pendingAuthorizations;
    }
    resetIdleTimer();
}

/**
 * @brief PolicyKitによる認可を要求する
 *
 * SubjectはSystemBusNameを用いる
 * UnixProcess (PIDベース) はPIDがレース中に再割り当てされるTOCTOU脆弱性 (CVE-2013-4288) があり、polkit自身も非推奨としている
 * SystemBusNameはカーネルのD-Bus name-owner情報をpolkitdが参照するため、呼び出し元の取り違えが起きない
 *
 * 対話を許可しない問い合わせを先に行う (高速経路)
 * allow_active=yesやauth_admin_keepのキャッシュ済み認可はここで許可となり、プロンプトが出ないためevent loopはミリ秒しか止まらない
 * 対話が必要な場合のみ非同期APIへ回す
 * 同期版の対話呼び出しはタイムアウトを持たず、任意のローカルユーザが未応答のプロンプトを1つ開くだけでrootサービスのevent loop全体を無期限に凍結できるため使用しない
 *
 * @param actionId チェックするアクションID
 * @param continuation 認可完了時に呼ぶ継続
 * @return 即時許可 / 即時拒否 / 遅延のいずれか
 */
SnapshotOperations::AuthorizationOutcome SnapshotOperations::beginAuthorization(
    const QString &actionId,
    std::function<void(const CallReply &, bool)> continuation)
{
    resetIdleTimer();

    const CallReply reply = captureCallReply();
    if (!reply.fromDBus || reply.message.service().isEmpty()) {
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

    PolkitAuthority *authority = polkitAuthorityInstance();
    if (!authority) {
        replyError(QDBusError::Failed,
                   QStringLiteral("Authorization service is unavailable"));
        return AuthorizationOutcome::Denied;
    }

    PolkitSubject *subject =
        polkit_system_bus_name_new(reply.message.service().toUtf8().constData());
    if (!subject) {
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

    GError *error = nullptr;
    PolkitAuthorizationResult *immediate = polkit_authority_check_authorization_sync(
        authority, subject, actionId.toUtf8().constData(), nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE, nullptr, &error);

    bool authorized = false;
    bool challenge = false;
    if (immediate) {
        authorized = polkit_authorization_result_get_is_authorized(immediate);
        challenge = polkit_authorization_result_get_is_challenge(immediate);
        g_object_unref(immediate);
    }
    else {
        qWarning() << "Polkit authorization pre-check failed:"
                   << (error ? error->message : "unknown error");
    }
    if (error) {
        g_error_free(error);
    }

    if (authorized) {
        g_object_unref(subject);
        return AuthorizationOutcome::Granted;
    }

    if (!challenge) {
        // 拒否が確定している (対話しても結果が変わらない) か、polkitdへ到達できなかった
        g_object_unref(subject);
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

    if (m_pendingAuthorizations >= MaxPendingAuthorizations) {
        g_object_unref(subject);
        replyError(QDBusError::LimitsExceeded,
                   QStringLiteral("Too many pending authorization requests"));
        return AuthorizationOutcome::Denied;
    }

    // 認可要求ごとに固有のuser_dataを渡し、完了結果が発行元の呼び出しへ
    // 一意に紐づくようにする
    auto *request = new AsyncAuthorization;
    request->service = this;
    request->continuation = [this, reply, continuation](bool granted) {
        endPendingAuthorization();
        continuation(reply, granted);
    };

    polkit_authority_check_authorization(
        authority, subject, actionId.toUtf8().constData(), nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, nullptr,
        asyncAuthorizationFinished, request);

    // polkit_authority_check_authorization()はsubjectを同期的にGVariant化するため、
    // 呼び出し直後に解放してよい
    g_object_unref(subject);

    ++m_pendingAuthorizations;
    setDelayedReply(true);
    // プロンプト応答待ちの間にアイドル終了しないようタイマを止める
    m_idleTimer.stop();
    return AuthorizationOutcome::Deferred;
}

/**
 * @brief Snapperインスタンスを取得
 *
 * 指定された設定名でSnapperインスタンスを取得または作成する
 * 設定が変更された場合は新しいインスタンスを作成する
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
            // Snapper差し替え前にキャッシュを無効化し、
            // 古いSnapperを指すComparisonが残らない (mount/Filesの寿命が古Snapperに依存する) ようにする
            m_comparisonCache.clear();
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
 * snapperライブラリのスナップショットタイプ列挙値を文字列表現に変換する
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
 * 文字列表現をsnapperライブラリのスナップショットタイプ列挙値に変換する
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
 * Snapperインスタンスから取得したスナップショット一覧をCSV形式の文字列に変換する
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
 * libsnapperのgetConfigs()を呼び出し、存在する全Snapper設定 (例: "root", "home") の設定名を抽出して配列で返す
 * スナップショット本体は返さない
 * PolicyKit認証 (list-snapshots) を必要とする
 *
 * @return 設定名の配列、失敗時は空配列
 */
QStringList SnapshotOperations::ListConfigs()
{
    return authorizeThen<QStringList>(
        QStringLiteral("com.presire.qsnapper.list-snapshots"),
        [this]() { return listConfigsAuthorized(); });
}

/**
 * @brief 認可済みのListConfigs本体
 * @return 設定名の配列、失敗時は空配列
 */
QStringList SnapshotOperations::listConfigsAuthorized()
{
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
 * 空文字列のconfigNameは"root"と解釈される
 * 呼び出し毎にSnapperインスタンスを強制再構築し、外部 (snapperd/snapper CLI等) で作成された新規スナップショットを確実に反映する
 * PolicyKit認証 (list-snapshots) を必要とする
 *
 * @param configName Snapper設定名 (空文字列時は"root")
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::ListSnapshots(const QString &configName)
{
    // resolveConfigOrFailが空文字列を "root" に正規化した上で検証する
    // 失敗時はD-Busエラー応答が送出済み
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.list-snapshots"),
        [this, config = *cfg]() { return listSnapshotsAuthorized(config); });
}

/**
 * @brief 認可済みのListSnapshots本体
 * @param configName 検証済みSnapper設定名
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::listSnapshotsAuthorized(const QString &configName)
{
    try {
        // 一覧取得時は必ず再構築して外部で作成された最新スナップショットを反映する
        snapper::Snapper *snapper = getSnapper(configName, /*forceReload=*/true);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        return formatSnapshotToCSV(snapper);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to list snapshots: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 新しいスナップショットを作成
 *
 * 指定されたパラメータで新しいスナップショットを作成する
 * single、pre、postの3種類のタイプをサポートする
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

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.create-snapshot"),
        [this, config = *cfg, type, description, preNumber, cleanup, userdata,
         important]() {
            return createSnapshotAuthorized(config, type, description, preNumber,
                                            cleanup, userdata, important);
        });
}

/**
 * @brief 認可済みのCreateSnapshot本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::createSnapshotAuthorized(
    const QString &configName, const QString &type, const QString &description,
    int preNumber, const QString &cleanup,
    const QMap<QString, QString> &userdata, bool important)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // スナップショット作成 (スナップショット一覧の変化) 前にComparisonキャッシュを無効化
        m_comparisonCache.clear();

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
                replyError(QDBusError::Failed, "Pre-snapshot not found");
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
        replyError(QDBusError::Failed, QString("Failed to create snapshot: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 既存スナップショットのメタデータを編集
 *
 * description / cleanup algorithm / userdata を差し替える
 * 空文字列("")のdescriptionはそのまま空文字列で上書きされる
 * userdataは渡されたマップで完全に置き換わる (差分ではない)
 * PolicyKit認証を必要とする
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

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.modify-snapshot"),
        [this, config = *cfg, number, description, cleanup, userdata]() {
            return modifySnapshotAuthorized(config, number, description, cleanup,
                                            userdata);
        });
}

/**
 * @brief 認可済みのModifySnapshot本体
 *
 * @param configName 検証済みSnapper設定名
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::modifySnapshotAuthorized(
    const QString &configName, int number, const QString &description,
    const QString &cleanup, const QMap<QString, QString> &userdata)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショット属性変更前にComparisonキャッシュを無効化
        m_comparisonCache.clear();

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
        replyError(QDBusError::Failed, QString("Failed to modify snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief スナップショットを削除する (D-Busスロット)
 *
 * Polkit認可は毎回 beginAuthorization()に委ねる
 * 連続削除時の再入力はpolkitのauth_admin_keep設定により、short-lived cookieで抑止される
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

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.delete-snapshot"),
        [this, config = *cfg, number]() {
            return deleteSnapshotAuthorized(config, number);
        });
}

/**
 * @brief 認可済みのDeleteSnapshot本体
 *
 * @param configName 検証済み設定名
 * @param number 削除対象スナップショット番号
 * @return 成功時true
 */
bool SnapshotOperations::deleteSnapshotAuthorized(const QString &configName,
                                                  int number)
{
    resetIdleTimer();

    // 実行中の復元計画が復元元として参照しているスナップショットは削除しない。
    // pin済みdirfdがソース読み取りの同一性を保証するが、復元中の削除は
    // 「認可時に存在した復元元」の消失につながるため、ここで明示的に拒否する
    for (const RestoreExecution &executionItem : m_restoreExecutions) {
        if (executionItem.configName == configName
                && executionItem.snapshotNumber == number) {
            replyError(QDBusError::Failed,
                           QStringLiteral("Snapshot is in use by an active restore plan"));
            return false;
        }
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショット削除 (スナップショット一覧の変化) 前にComparisonキャッシュを無効化
        m_comparisonCache.clear();

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
        replyError(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        resetIdleTimer();   // 例外時もタイマリセット
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時にそのスナップショットの状態で起動する
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

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.rollback-snapshot"),
        [this, config = *cfg, number]() {
            return rollbackSnapshotAuthorized(config, number);
        });
}

/**
 * @brief 認可済みのRollbackSnapshot本体
 *
 * @param configName 検証済み設定名
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時: true、失敗時: false
 */
bool SnapshotOperations::rollbackSnapshotAuthorized(const QString &configName,
                                                    int number)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots &snapshots = snapper->getSnapshots();
        snapper::Snapshots::iterator target = snapshots.find(number);
        if (target == snapshots.end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // ロールバックはスナップショット一覧・現在状態を変化させるためComparisonキャッシュを無効化
        m_comparisonCache.clear();

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
        replyError(QDBusError::Failed, QString("Failed to rollback snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief ファイル変更一覧を取得
 *
 * 指定されたスナップショットと現在のシステム状態を比較し、変更されたファイルの一覧を取得する
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

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, snapshotNumber]() {
            return getFileChangesAuthorized(config, snapshotNumber);
        });
}

/**
 * @brief 認可済みのGetFileChanges本体
 *
 * @param configName 検証済みSnapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::getFileChangesAuthorized(const QString &configName,
                                                     int snapshotNumber)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // snapshot1: 比較元 (指定されたスナップショット)
        // snapshot2: 比較先 (現在のシステム状態)
        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを作成してファイル変更を取得
        // snapshot1からsnapshot2への変更を取得
        // mount=trueでComparisonを構築し、後続する詳細取得APIがmountを再利用できるようにする
        // Refresh戦略: 一覧取得時に前回キャッシュを破棄して最新状態を反映する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, snapshotNumber, std::nullopt},
            CachePolicy::Refresh,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

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

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間のファイル変更リストを取得
 *
 * snapshot1 -> snapshot2の差分を取得する
 * 現在のシステム状態は使用しない
 */
QString SnapshotOperations::GetFileChangesBetween(const QString &configName, int number1, int number2)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, number1, number2]() {
            return getFileChangesBetweenAuthorized(config, number1, number2);
        });
}

/**
 * @brief 認可済みのGetFileChangesBetween本体
 *
 * @param configName 検証済みSnapper設定名
 * @param number1 比較元スナップショット番号
 * @param number2 比較先スナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::getFileChangesBetweenAuthorized(
    const QString &configName, int number1, int number2)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // mount=trueでComparisonを構築し、後続する詳細取得APIがmountを再利用できるようにする
        // Refresh戦略: 一覧取得時に前回キャッシュを破棄して最新状態を反映する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, number1, number2},
            CachePolicy::Refresh,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

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
        replyError(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間の個別ファイルの詳細 + diffを取得
 *
 * snapshot1側のパーミッションとsnapshot2側のパーミッションを返し、diff部も両snapshot上のファイルを比較する
 */
QString SnapshotOperations::GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!qsnapper::security::validateAbsoluteFilePath(filePath)) {
        replyError(QDBusError::InvalidArgs, "Invalid file path");
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, number1, number2, filePath]() {
            return getFileDiffBetweenAuthorized(config, number1, number2,
                                                filePath);
        });
}

/**
 * @brief 認可済みのGetFileDiffBetween本体
 *
 * @param configName 検証済みSnapper設定名
 * @param number1 比較元スナップショット番号
 * @param number2 比較先スナップショット番号
 * @param filePath 検証済み対象ファイルの絶対path
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::getFileDiffBetweenAuthorized(
    const QString &configName, int number1, int number2, const QString &filePath)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Reuse戦略: キーが一致すればリスト取得時に構築したmount有効Comparisonを再利用し、
        // ミス時は新規構築 (mount=true) してキャッシュに格納する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, number1, number2},
            CachePolicy::Reuse,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

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

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        // snapshot1をLOC_PREとして扱い、snapshot2をLOC_POSTとして扱う
        QString path1 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        struct stat info1;
        const bool hasInfo1 = qsnapper::security::safeLstat(path1, &info1);
        if (hasInfo1) {
            detailsPart += "snapshotPerms=" + permsToOctal(info1.st_mode) + "\n";
            detailsPart += "snapshotOwner=" + ownerName(info1.st_uid) + "\n";
            detailsPart += "snapshotGroup=" + groupName(info1.st_gid) + "\n";
        }

        QString path2 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_POST));
        struct stat info2;
        const bool hasInfo2 = qsnapper::security::safeLstat(path2, &info2);
        if (hasInfo2) {
            detailsPart += "currentPerms=" + permsToOctal(info2.st_mode) + "\n";
            detailsPart += "currentOwner=" + ownerName(info2.st_uid) + "\n";
            detailsPart += "currentGroup=" + groupName(info2.st_gid) + "\n";
        }

        QString diffPart;
        if (hasInfo1 && hasInfo2) {
            diffPart = generateUnifiedDiff(path1, path2);
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff between snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルの差分と詳細情報を一括取得
 *
 * 1回のComparisonオブジェクト生成で、差分 (diff) と 詳細情報 (パーミッション等) の両方を取得する
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

    if (!qsnapper::security::validateAbsoluteFilePath(filePath)) {
        replyError(QDBusError::InvalidArgs, "Invalid file path");
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, snapshotNumber, filePath]() {
            return getFileDiffAndDetailsAuthorized(config, snapshotNumber,
                                                   filePath);
        });
}

/**
 * @brief 認可済みのGetFileDiffAndDetails本体
 *
 * @param configName 検証済みSnapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 検証済み対象ファイルの絶対path
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::getFileDiffAndDetailsAuthorized(
    const QString &configName, int snapshotNumber, const QString &filePath)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Reuse戦略: キーが一致すればリスト取得時に構築したmount有効Comparisonを再利用し、
        // ミス時は新規構築 (mount=true) してキャッシュに格納する
        // Comparisonオブジェクトは1回だけ作成 (スナップショットマウントも1回のみ)
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, snapshotNumber, std::nullopt},
            CachePolicy::Reuse,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        // Details部の構築
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

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        QString snapshotPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        struct stat snapshotInfo;
        const bool hasSnapshotInfo = qsnapper::security::safeLstat(snapshotPath, &snapshotInfo);
        if (hasSnapshotInfo) {
            detailsPart += "snapshotPerms=" + permsToOctal(snapshotInfo.st_mode) + "\n";
            detailsPart += "snapshotOwner=" + ownerName(snapshotInfo.st_uid) + "\n";
            detailsPart += "snapshotGroup=" + groupName(snapshotInfo.st_gid) + "\n";
        }

        QString currentPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_SYSTEM));
        struct stat currentInfo;
        const bool hasCurrentInfo = qsnapper::security::safeLstat(currentPath, &currentInfo);
        if (hasCurrentInfo) {
            detailsPart += "currentPerms=" + permsToOctal(currentInfo.st_mode) + "\n";
            detailsPart += "currentOwner=" + ownerName(currentInfo.st_uid) + "\n";
            detailsPart += "currentGroup=" + groupName(currentInfo.st_gid) + "\n";
        }

        // Diff部の取得
        QString diffPart;
        if (hasSnapshotInfo && hasCurrentInfo) {
            diffPart = generateUnifiedDiff(snapshotPath, currentPath);
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff and details:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff and details: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ownerに束縛された空のstaged restore計画を開始する
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元snapshot番号
 * @param restoreMode yastまたはdirect
 * @return 成功時manifest id
 */
QString SnapshotOperations::BeginRestorePlan(const QString &configName,
                                             int snapshotNumber,
                                             const QString &restoreMode)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
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
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid restore mode"));
        return {};
    }

    if (snapshotNumber <= 0) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid snapshot number"));
        return {};
    }

    purgeExpiredRestorePlans();

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const QString manifestId = m_restoreRegistry.createStaging(
        owner, *cfg, snapshotNumber, mode, &error);
    if (manifestId.isEmpty()) {
        sendManifestError(error);
        return {};
    }

    m_restorePlanOwners.insert(manifestId, owner);
    if (m_ownerWatcher && !owner.isEmpty()
            && !m_ownerWatcher->watchedServices().contains(owner)) {
        m_ownerWatcher->addWatchedService(owner);
    }
    return manifestId;
}

/**
 * @brief staging計画へ検証済みentry chunkを原子的に追加する
 * @param manifestId owner束縛されたmanifest id
 * @param filePaths 復元対象絶対path列
 * @param changeTypes pathと対応する変更種別列
 * @return chunk全体を追加できた場合true
 */
bool SnapshotOperations::StageRestoreEntries(
    const QString &manifestId,
    const QStringList &filePaths,
    const QStringList &changeTypes)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    // stagingは認可を要さないため、グローバル予算を消費する唯一の経路でもある。
    // 失効済み計画をここで回収しておかないと、放置された計画が予算を占有し続け、
    // 正規の利用者がGlobalLimitで弾かれる
    purgeExpiredRestorePlans();

    if (filePaths.size() != changeTypes.size()) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Restore entry lists must have the same size"));
        return false;
    }
    if (filePaths.isEmpty()) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Restore entry chunk is empty"));
        return false;
    }
    if (filePaths.size()
            > qsnapper::restore::RestoreManifestRegistry::kMaxEntriesPerStageChunk) {
        replyError(QDBusError::InvalidArgs,
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
            replyError(QDBusError::InvalidArgs,
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
 * @brief 計画をfreeze後に1度だけ認可し非同期実行を開始する
 * @param manifestId owner束縛されたmanifest id
 * @return 実行開始を受理した場合true
 */
bool SnapshotOperations::CommitRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
                       QStringLiteral("Restore plan caller is unavailable"));
        return false;
    }

    // 復元はlive filesystemを書き換える排他的な操作である。
    // 複数計画がchunk境界で交互実行されると最終状態が非決定になり、
    // libsnapperのmount_user_requestがbool (カウンタではない) であることも
    // 相まってmount管理が衝突し得るため、同時に1計画のみ実行を許す
    if (!m_restoreExecutions.isEmpty()) {
        replyError(QDBusError::Failed,
                       QStringLiteral("Another restore plan is already running"));
        return false;
    }

    purgeExpiredRestorePlans();

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

    // 認可対象は凍結済みの不変計画である。
    // 認可待ちの間にcancel / TTL失効 / owner消失 / 他計画の実行開始が起こり得るため、
    // 実際のmountとexecutor起動は継続側で状態を再検証してから行う
    const AuthorizationOutcome outcome = beginAuthorization(
        QStringLiteral("com.presire.qsnapper.rollback-snapshot"),
        [this, manifestId, owner](const CallReply &reply, bool granted) {
            m_deferredReply = reply;
            bool accepted = false;
            if (granted) {
                accepted = commitRestorePlanAuthorized(manifestId, owner);
            }
            else {
                failRestorePlanAuthorization(manifestId, owner);
                replyError(QDBusError::AccessDenied,
                           QStringLiteral("Authorization failed"));
            }

            const bool alreadyReplied = m_deferredReply->replied;
            m_deferredReply.reset();
            if (!alreadyReplied) {
                reply.connection.send(
                    reply.message.createReply(QVariant::fromValue(accepted)));
            }
        });

    switch (outcome) {
    case AuthorizationOutcome::Granted:
        return commitRestorePlanAuthorized(manifestId, owner);
    case AuthorizationOutcome::Denied:
        // beginAuthorizationがD-Busエラー応答を送出済み
        failRestorePlanAuthorization(manifestId, owner);
        return false;
    case AuthorizationOutcome::Deferred:
        break;
    }
    return false;
}

/**
 * @brief 認可が得られなかった復元計画をFailedで終端する
 * @param manifestId 対象manifest id
 * @param owner 認可前にcaptureした呼び出し元unique name
 */
void SnapshotOperations::failRestorePlanAuthorization(const QString &manifestId,
                                                      const QString &owner)
{
    qsnapper::restore::ManifestError failureError =
        qsnapper::restore::ManifestError::None;
    m_restoreRegistry.markFailed(
        manifestId, owner, QStringLiteral("Authorization failed"),
        &failureError);
}

/**
 * @brief 認可済みのCommitRestorePlan本体
 *
 * 認可待ちの間にevent loopが回るため、凍結済み計画が生き残っている保証はない。
 * mountやexecutor起動といった不可逆な操作の前に、以下を必ず再検証する:
 *   - 他の計画が実行を開始していないこと (復元はlive filesystemへの排他操作)
 *   - 計画がownerに束縛されたまま存在し、まだFrozenであること
 *     (cancel / TTL失効 / owner消失は全てここで弾かれる)
 *
 * @param manifestId owner束縛されたmanifest id
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @return 実行開始を受理した場合true
 */
bool SnapshotOperations::commitRestorePlanAuthorized(const QString &manifestId,
                                                     const QString &owner)
{
    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;

    // 認可待ちの間に別の計画がcommitされている可能性がある
    if (!m_restoreExecutions.isEmpty()) {
        failRestorePlanAuthorization(manifestId, owner);
        replyError(QDBusError::Failed,
                   QStringLiteral("Another restore plan is already running"));
        return false;
    }

    // 認可待ちの間のcancel / TTL失効 / owner消失 / purgeを検出する
    const auto status = m_restoreRegistry.status(manifestId, owner, &error);
    if (!status) {
        return sendManifestError(error);
    }
    if (status->state != qsnapper::restore::ManifestState::Frozen) {
        return sendManifestError(qsnapper::restore::ManifestError::WrongState);
    }

    RestoreExecution execution;
    execution.owner = owner;
    execution.configName = status->configName;
    execution.snapshotNumber = status->snapshotNumber;
    execution.useReflink =
        status->mode == qsnapper::restore::RestoreMode::DirectCopy;
    execution.removeOnTypechanged = execution.useReflink;

    try {
        snapper::Snapper *snapper = getSnapper(execution.configName);
        if (!snapper) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Failed to initialize restore source"),
                &error);
            replyError(QDBusError::Failed,
                           QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        const snapper::Snapshots::const_iterator snapshot =
            snapper->getSnapshots().find(execution.snapshotNumber);
        if (snapshot == snapper->getSnapshots().end()) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore source snapshot is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                           QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        m_comparisonCache.clear();
        snapshot->mountFilesystemSnapshot(true);
        execution.mounted = true;
        const QString snapshotDir = QString::fromStdString(snapshot->snapshotDir());
        m_restoreExecutions.insert(manifestId, execution);
        m_restoreExecutions[manifestId].snapshotDir = snapshotDir;

        // ソースsnapshotをdirfdでpinする。
        // 以降のソース読み取りは本fd相対で行うため、chunk境界でevent loopが回る間に
        // path上のsnapshotが削除 / 同番号で再作成 / 差し替えられても、
        // 認可時に参照したsnapshotそのものから復元し続ける
        const int snapshotDirFd = ::open(snapshotDir.toUtf8().constData(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (snapshotDirFd < 0) {
            qWarning() << "Staged restore: Failed to pin snapshot dir:"
                       << strerror(errno);
            cleanupRestoreExecution(manifestId);
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Failed to pin restore source"), &error);
            replyError(QDBusError::Failed,
                           QStringLiteral("Failed to prepare restore plan"));
            return false;
        }
        m_restoreExecutions[manifestId].snapshotDirFd = snapshotDirFd;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Staged restore preparation failed:" << e.what();
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                       QStringLiteral("Failed to prepare restore plan"));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << "Staged restore preparation failed unexpectedly:"
                   << e.what();
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                       QStringLiteral("Failed to prepare restore plan"));
        return false;
    }
    catch (...) {
        qWarning() << "Staged restore preparation failed unexpectedly";
        cleanupRestoreExecution(manifestId);
        m_restoreRegistry.markFailed(
            manifestId, owner, QStringLiteral("Failed to prepare restore source"),
            &error);
        replyError(QDBusError::Failed,
                       QStringLiteral("Failed to prepare restore plan"));
        return false;
    }

    if (!m_restoreExecutor.start(manifestId, owner, &error)) {
        cleanupRestoreExecution(manifestId);
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
bool SnapshotOperations::ContinueRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
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
QString SnapshotOperations::GetRestorePlanStatus(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
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
bool SnapshotOperations::CancelRestorePlan(const QString &manifestId)
{
    resetIdleTimer();

    const QString owner = callerOwner();
    if (calledFromDBus() && owner.isEmpty()) {
        replyError(QDBusError::AccessDenied,
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

/**
 * @brief ファイルをスナップショットから復元する (YaST互換経路)
 *
 * 内部で restoreFilesImpl を呼び出すだけのラッパー
 * reflinkは使用せず、typechangedの事前削除も行わない
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
 * btrfs reflink (FICLONE) を優先し、typechanged時は既存ファイルを削除してから上書きする
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
 *   - configNameは本関数冒頭で resolveConfigOrFail() により正規化・検証すること
 *   - filePathsの各要素は絶対パスで、かつ snapshotDir配下を指すこと
 *     (snapshotFilePath = snapshotDir + filePathがsnapshotDir内に収まることをisPathWithinSnapshotRootで検証)
 *   - "/.snapshots/" 直下への書き込み (systemFilePath側) は書き込み対象として棄却する
 *   - シンボリックリンク解決は copySymlink / copyRegularFileのレイヤーで行う
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

    // 入力検証は認可より前に行う。
    // polkitプロンプトを出してから "No files specified" で蹴るUXを避けるとともに、
    // 攻撃者が不正な入力でpolkitを浪費するのを防ぐ
    if (filePaths.isEmpty()) {
        replyError(QDBusError::InvalidArgs, "No files specified for restore");
        return false;
    }

    if (filePaths.size() != changeTypes.size()) {
        replyError(QDBusError::InvalidArgs, "filePaths and changeTypes must have the same size");
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.rollback-snapshot"),
        [this, config = *cfg, snapshotNumber, filePaths, changeTypes, useReflink,
         removeOnTypechanged, logTag]() {
            return restoreFilesAuthorized(config, snapshotNumber, filePaths,
                                          changeTypes, useReflink,
                                          removeOnTypechanged, logTag);
        });
}

/**
 * @brief 認可済みのrestoreFilesImpl本体
 *
 * configName / filePaths / changeTypes の検証はrestoreFilesImpl側で認可前に完了している
 *
 * @param configName 検証済みSnapper設定名
 * @return 全ファイル成功時true
 */
bool SnapshotOperations::restoreFilesAuthorized(const QString &configName,
                                                int snapshotNumber,
                                                const QStringList &filePaths,
                                                const QStringList &changeTypes,
                                                bool useReflink,
                                                bool removeOnTypechanged,
                                                const char *logTag)
{
    qInfo() << logTag << ": Starting restore for" << filePaths.size()
            << "files from snapshot" << snapshotNumber
            << "(useReflink=" << useReflink
            << ", removeOnTypechanged=" << removeOnTypechanged << ")";

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        if (snapshot1 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // 復元操作は現在システム状態を変化させるためComparisonキャッシュを無効化
        m_comparisonCache.clear();

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
            const bool validChangeType = changeType == QStringLiteral("created")
                    || changeType == QStringLiteral("deleted")
                    || changeType == QStringLiteral("modified")
                    || changeType == QStringLiteral("typechanged");

            // 入力検証 (進捗 emit より前に行い、未検証パスをD-Busシグナルへ漏出させない)
            // (1) 絶対パスでなければ拒否
            if (!filePath.startsWith(QLatin1Char('/'))) {
                qWarning() << logTag << ": Rejecting non-absolute path:" << filePath;
                skippedCount++;
                continue;
            }

            // (2) 書き込み先として /.snapshots とその配下は禁止 (スナップショット木の破壊防止)
            if (filePath == QStringLiteral("/.snapshots")
                    || filePath.startsWith(QStringLiteral("/.snapshots/"))) {
                qWarning() << logTag << ": Skipping dangerous destination path:" << filePath;
                skippedCount++;
                continue;
            }

            // (3) 変更種別は既知のallowlistのみ許可
            if (!validChangeType) {
                qWarning() << logTag << ": Rejecting unknown change type:" << changeType;
                skippedCount++;
                continue;
            }

            // (4) snapshotDir + filePathがsnapshotDir配下に収まっていること
            //     (".."を含むfilePathによるsnapshotツリー外参照を防ぐ)
            const QString snapshotFilePath = snapshotDir + filePath;
            if (!qsnapper::security::isPathWithinSnapshotRoot(snapshotFilePath, snapshotDir)) {
                qWarning() << logTag << ": Rejecting path escaping snapshot root:" << filePath;
                skippedCount++;
                continue;
            }

            // 検証通過後にのみ進捗を通知 (D-Busシグナルが運ぶのは受理済みパスのみ)
            emit restoreProgress(i + 1, total, QFileInfo(filePath).fileName());

            // システム上のファイルパス (ルートからの絶対パス)
            const QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル --> 削除
                fileSuccess = qsnapper::security::safeRemoveAll(systemFilePath);
                if (!fileSuccess) {
                    qWarning() << logTag << ": Failed to remove" << systemFilePath
                               << strerror(errno);
                }
            }
            else {
                // deleted / modified / typechanged --> スナップショットからコピー

                // 親ディレクトリを確認・作成
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty() && !qsnapper::security::safeMkpath(parentDir)) {
                    qWarning() << logTag << ": Failed to safely create parent directory"
                               << parentDir << strerror(errno);
                    allSuccess = false;
                    continue;
                }

                struct stat snapshotFileInfo;
                const bool hasSnapshotFileInfo = qsnapper::security::safeLstat(snapshotFilePath, &snapshotFileInfo);

                // live側を破壊する前に復元元の可用性と種別を確定させる。
                // 先に退避・削除してから "Source not restorable" で失敗すると、
                // 復元元が存在しないままlive側のデータだけが失われる
                const bool sourceIsLink = hasSnapshotFileInfo && S_ISLNK(snapshotFileInfo.st_mode);
                const bool sourceIsDirectory = hasSnapshotFileInfo && S_ISDIR(snapshotFileInfo.st_mode);
                const bool sourceIsRegular = hasSnapshotFileInfo && S_ISREG(snapshotFileInfo.st_mode);
                if (!sourceIsLink && !sourceIsDirectory && !sourceIsRegular) {
                    qWarning() << logTag << ": Source not restorable from snapshot:" << snapshotFilePath;
                    allSuccess = false;
                    continue;
                }

                QString detachedPath;
                if (removeOnTypechanged && changeType == "typechanged") {
                    if (!movePathAsideNoFollow(systemFilePath, &detachedPath)) {
                        qWarning() << logTag << ": Failed to move existing path aside before restore"
                                   << systemFilePath << strerror(errno);
                        allSuccess = false;
                        continue;
                    }
                }

                if (sourceIsLink) {
                    // シンボリックリンクの場合
                    fileSuccess = copySymlink(snapshotFilePath, systemFilePath);
                    if (!fileSuccess) {
                        qWarning() << logTag << ": Failed to copy symlink" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else if (sourceIsDirectory) {
            // ディレクトリの場合: safeMkpath + safeOpenDirectory で取得した dirFd に対して fchown/fchmod
            if (!qsnapper::security::safeMkpath(systemFilePath)) {
                        qWarning() << logTag << ": Failed to safely create directory"
                                   << systemFilePath;
                        fileSuccess = false;
                    }
                    else {
                        const int dirFd = qsnapper::security::safeOpenDirectory(systemFilePath);
                        if (dirFd < 0) {
                            qWarning() << logTag << ": Failed to open directory safely"
                                       << systemFilePath << strerror(errno);
                            fileSuccess = false;
                        }
                        else {
                            // パーミッションをコピー (snapshot から lstat した stat を、live dir の fd に対して fchown/fchmod)
                            struct stat st;
                            if (qsnapper::security::safeLstat(snapshotFilePath, &st)) {
                                const bool mustPreserveMetadata = (::geteuid() == 0);
                                bool metadataOk = true;
                                if (::fchown(dirFd, st.st_uid, st.st_gid) < 0) {
                                    qWarning() << logTag << ": Failed to preserve directory owner"
                                               << systemFilePath << strerror(errno);
                                    metadataOk = !mustPreserveMetadata;
                                }
                                if (::fchmod(dirFd, st.st_mode & 07777) < 0) {
                                    qWarning() << logTag << ": Failed to preserve directory mode"
                                               << systemFilePath << strerror(errno);
                                    metadataOk = metadataOk && !mustPreserveMetadata;
                                }
                                ::close(dirFd);
                                fileSuccess = metadataOk;
                            }
                            else {
                                ::close(dirFd);
                                fileSuccess = false;
                            }
                        }
                    }
                }
                else {
                    // 通常ファイルの場合 (useReflink=trueならFICLONEを先行試行)
                    fileSuccess = copyRegularFile(snapshotFilePath, systemFilePath, useReflink);
                    if (!fileSuccess) {
                        qWarning() << logTag << ": Failed to copy" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }

                // 退避物の破棄は復元成功後にのみ行う。
                // 失敗時は元の位置へ戻し、復元できなかったlive側のデータを消さない。
                // 戻すことすらできない場合も退避物は削除せず、復旧できるようpathを記録する
                if (!detachedPath.isEmpty()) {
                    if (fileSuccess) {
                        if (!qsnapper::security::safeRemoveAll(detachedPath)) {
                            qWarning() << logTag << ": Failed to remove detached path after restore"
                                       << detachedPath;
                        }
                    }
                    else if (!qsnapper::security::safeRenamePathNoFollow(detachedPath, systemFilePath)) {
                        qCritical() << logTag << ": Failed to reattach live path after a failed restore."
                                    << "Previous content is preserved at:" << detachedPath;
                    }
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
                const auto rwResult = btrfs_util_set_subvolume_read_only("/", false);
                if (rwResult != BTRFS_UTIL_OK) {
                    qCritical() << logTag << ": Failed to restore root subvolume rw state:" << rwResult;
                    allSuccess = false;
                }
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
            replyError(QDBusError::Failed, errorMsg);
        }

        return allSuccess;
    }
    catch (const snapper::Exception &e) {
        qWarning() << logTag << " failed:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to restore files: %1").arg(e.what()));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << logTag << " unexpected error:" << e.what();
        replyError(QDBusError::Failed, QString("Unexpected error: %1").arg(e.what()));
        return false;
    }
}

/**
    * @brief 通常ファイルをコピー (safeOpenRegularFile* + sendfile + fd-based 所有者/権限/タイムスタンプ保持)
 *
 * tryReflink=trueの場合、まずioctl(FICLONE)を試行し、
 * btrfs CoW (reflink)が使用可能であれば高速コピー、失敗時はsendfileにフォールバック
 *
 * "cp -d --preserve=all --no-preserve=xattr"と同等の動作
 */
bool SnapshotOperations::copyRegularFile(const QString &src, const QString &dst, bool tryReflink)
{
    int srcFd = qsnapper::security::safeOpenRegularFileRead(src);
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

    int dstFd = qsnapper::security::safeOpenRegularFileWrite(dst, srcStat.st_mode & 07777);
    if (dstFd < 0) {
        qWarning() << "copyRegularFile: Failed to open destination:" << dst << strerror(errno);
        close(srcFd);
        return false;
    }

    bool copied = false;

    // Step 1: reflink (btrfs CoW) を試行
    if (tryReflink) {
        if (ioctl(dstFd, FICLONE, srcFd) == 0) {
            copied = true;
        }
        // FICLONE失敗時はsendfileにフォールバック
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
            if (written == 0) {
                qWarning() << "copyRegularFile: sendfile reached EOF before expected byte count";
                close(dstFd);
                close(srcFd);
                return false;
            }
            remaining -= written;
        }
    }

    // 所有者を保持 (cp --preserve=all)
    const bool mustPreserveMetadata = (::geteuid() == 0);
    if (fchown(dstFd, srcStat.st_uid, srcStat.st_gid) < 0) {
        qWarning() << "copyRegularFile: fchown failed" << strerror(errno);
        if (mustPreserveMetadata) {
            close(dstFd);
            close(srcFd);
            return false;
        }
    }

    // タイムスタンプを保持
    struct timespec ts[2];
    ts[0] = srcStat.st_atim;
    ts[1] = srcStat.st_mtim;
    if (futimens(dstFd, ts) < 0) {
        qWarning() << "copyRegularFile: futimens failed" << strerror(errno);
        if (mustPreserveMetadata) {
            close(dstFd);
            close(srcFd);
            return false;
        }
    }

    close(dstFd);
    close(srcFd);
    return true;
}

    /**
    * @brief シンボリックリンクをコピー (readlinkat → symlinkat → fchownat(AT_SYMLINK_NOFOLLOW) + utimensat(AT_SYMLINK_NOFOLLOW))
 *
 * copySymlinkは、リンク先自体を保持しつつ、名前の変更やメタデータの更新を信頼できる親ディレクトリファイルへの参照に固定するため、
 * 中間にある親ディレクトリでのライブパスシンボリックリンクの置換によって、最終的な操作がリダイレクトされることはない
 *
 * "cp -d --preserve=all --no-preserve=xattr"のシンボリックリンク版
 */
bool SnapshotOperations::copySymlink(const QString &src, const QString &dst)
{
    QByteArray linkTarget;
    if (!qsnapper::security::safeReadLinkNoFollow(src, &linkTarget)) {
        qWarning() << "copySymlink: readlink failed:" << src << strerror(errno);
        return false;
    }

    QString temporaryPath;
    bool createdTemporaryLink = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporaryPath = siblingTemporaryPath(dst, QStringLiteral("qsnapper-link"), attempt);
        if (qsnapper::security::safeCreateSymlinkNoFollow(linkTarget, temporaryPath)) {
            createdTemporaryLink = true;
            break;
        }

        if (errno != EEXIST) {
            qWarning() << "copySymlink: symlink(temp) failed:" << temporaryPath << strerror(errno);
            return false;
        }
    }

    if (!createdTemporaryLink) {
        qWarning() << "copySymlink: failed to allocate temporary link path:" << dst;
        return false;
    }

    if (!qsnapper::security::safeRenamePathNoFollow(temporaryPath, dst)) {
        const int renameErrno = errno;
        qsnapper::security::safeRemoveAll(temporaryPath);
        qWarning() << "copySymlink: rename failed:" << dst << strerror(renameErrno);
        return false;
    }

    struct stat srcStat;
    if (qsnapper::security::safeLstat(src, &srcStat)) {
        struct timespec ts[2];
        ts[0] = srcStat.st_atim;
        ts[1] = srcStat.st_mtim;

        bool ownerUpdated = false;
        bool timesUpdated = false;
        if (!qsnapper::security::safeSetSymlinkMetadataNoFollow(dst,
                                                                srcStat.st_uid,
                                                                srcStat.st_gid,
                                                                ts,
                                                                &ownerUpdated,
                                                                &timesUpdated)) {
            if (!ownerUpdated) {
                qWarning() << "copySymlink: fchownat failed (non-fatal):" << dst << strerror(errno);
            }

            if (!timesUpdated) {
                qWarning() << "copySymlink: utimensat failed (non-fatal):" << dst << strerror(errno);
            }
        }
    }

    return true;
}

/**
 * @brief live pathをroot配下で再解決して一時的な兄弟pathへ退避する
 * @param path 退避対象の絶対path
 * @param movedPath 実際の退避先。対象不存在時は空文字列
 * @return 退避または対象不存在時true
 */
bool SnapshotOperations::movePathAsideBeneathRoot(const QString &path,
                                                  QString *movedPath)
{
    if (movedPath) {
        movedPath->clear();
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = siblingTemporaryPath(
            path, QStringLiteral("qsnapper-old"), attempt);
        if (qsnapper::security::safeRenamePathNoFollowBeneathRoot(
                QStringLiteral("/"), path, candidate)) {
            if (movedPath) {
                *movedPath = candidate;
            }
            return true;
        }

        if (errno == ENOENT) {
            return true;
        }
        if (errno == EEXIST || errno == ENOTEMPTY) {
            continue;
        }
        return false;
    }

    errno = EEXIST;
    return false;
}

/**
 * @brief executor callbackから単一の凍結済みentryをlive filesystemへ適用する
 * @param manifestId 実行contextを選択するmanifest id
 * @param entry 適用対象entry
 * @return entry全体の適用成功時true
 */
bool SnapshotOperations::applyRestoreEntry(
    const QString &manifestId,
    const qsnapper::restore::RestoreEntry &entry)
{
    const auto execution = m_restoreExecutions.constFind(manifestId);
    if (execution == m_restoreExecutions.cend()) {
        qWarning() << "Staged restore: Missing execution context";
        return false;
    }
    const RestoreExecution context = execution.value();
    if (context.snapshotDirFd < 0) {
        qWarning() << "Staged restore: Snapshot source fd is not pinned";
        return false;
    }

    const bool validChangeType = entry.changeType == QStringLiteral("created")
            || entry.changeType == QStringLiteral("deleted")
            || entry.changeType == QStringLiteral("modified")
            || entry.changeType == QStringLiteral("typechanged");
    QString relativePath;
    if (!entry.path.startsWith(QLatin1Char('/'))
            || entry.path == QStringLiteral("/.snapshots")
            || entry.path.startsWith(QStringLiteral("/.snapshots/"))
            || !validChangeType
            || !qsnapper::security::splitDestinationBeneathRoot(
                QStringLiteral("/"), entry.path, &relativePath)) {
        qWarning() << "Staged restore: Frozen entry failed execution-time validation";
        return false;
    }

    const QString snapshotFilePath = context.snapshotDir + entry.path;
    if (!qsnapper::security::isPathWithinSnapshotRoot(
            snapshotFilePath, context.snapshotDir)) {
        qWarning() << "Staged restore: Source escaped snapshot root";
        return false;
    }

    if (entry.changeType == QStringLiteral("created")) {
        const bool removed = qsnapper::security::safeRemoveAllBeneathRoot(
            QStringLiteral("/"), entry.path);
        if (!removed) {
            qWarning() << "Staged restore: Failed to remove live path:"
                       << strerror(errno);
        }
        return removed;
    }

    const int slashIndex = entry.path.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = slashIndex <= 0
        ? QStringLiteral("/")
        : entry.path.left(slashIndex);
    if (parentPath != QStringLiteral("/")
            && !qsnapper::security::safeCreateDirectoryBeneathRoot(
                QStringLiteral("/"), parentPath, 0755)) {
        qWarning() << "Staged restore: Failed to create live parent directory:"
                   << strerror(errno);
        return false;
    }

    // ソースの種別判定はpin済みsnapshot dirfd相対で行う。
    // 実行中にsnapshotDirのpath上で何が起きても、認可時にpinしたinodeを観測する
    // (AT_SYMLINK_NOFOLLOWによりleafのsymlinkも展開しない)
    struct stat snapshotFileInfo;
    const bool hasSnapshotFileInfo = qsnapper::security::safeLstatAt(
        context.snapshotDirFd, relativePath, &snapshotFileInfo);

    // live側を破壊する前に復元元の可用性と種別を確定させる。
    // 凍結・認可の時点でsnapshotDirを検証していても、認可から本entryの適用までには
    // chunk境界でevent loopが回るため、その間に復元元snapshotが削除 / unmountされ得る。
    // 先に退避・削除してから "Source is not restorable" で失敗すると、
    // 復元元が存在しないままlive側のデータだけが失われる
    const bool sourceIsLink =
        hasSnapshotFileInfo && S_ISLNK(snapshotFileInfo.st_mode);
    const bool sourceIsDirectory =
        hasSnapshotFileInfo && S_ISDIR(snapshotFileInfo.st_mode);
    const bool sourceIsRegular =
        hasSnapshotFileInfo && S_ISREG(snapshotFileInfo.st_mode);
    if (!sourceIsLink && !sourceIsDirectory && !sourceIsRegular) {
        qWarning() << "Staged restore: Source is not restorable";
        return false;
    }

    QString detachedPath;
    if (context.removeOnTypechanged
            && entry.changeType == QStringLiteral("typechanged")) {
        if (!movePathAsideBeneathRoot(entry.path, &detachedPath)) {
            qWarning() << "Staged restore: Failed to move live path aside:"
                       << strerror(errno);
            return false;
        }
    }

    bool applied = false;
    if (sourceIsLink) {
        applied = copySymlinkBeneathRoot(context.snapshotDirFd, relativePath,
                                         entry.path);
    }
    else if (sourceIsDirectory) {
        if (!qsnapper::security::safeCreateDirectoryBeneathRoot(
                QStringLiteral("/"), entry.path, 0755)) {
            qWarning() << "Staged restore: Failed to create live directory:"
                       << strerror(errno);
        }
        else {
            const int directoryFd =
                qsnapper::security::safeOpenDirectoryBeneathRoot(
                    QStringLiteral("/"), relativePath,
                    /*createMissing=*/false, 0755);
            if (directoryFd < 0) {
                qWarning() << "Staged restore: Failed to open live directory:"
                           << strerror(errno);
            }
            else {
                const bool mustPreserveMetadata = (::geteuid() == 0);
                bool metadataOk = true;
                if (::fchown(directoryFd, snapshotFileInfo.st_uid,
                             snapshotFileInfo.st_gid) < 0) {
                    qWarning() << "Staged restore: Failed to preserve directory owner:"
                               << strerror(errno);
                    metadataOk = !mustPreserveMetadata;
                }
                if (::fchmod(directoryFd, snapshotFileInfo.st_mode & 07777) < 0) {
                    qWarning() << "Staged restore: Failed to preserve directory mode:"
                               << strerror(errno);
                    metadataOk = metadataOk && !mustPreserveMetadata;
                }
                ::close(directoryFd);
                applied = metadataOk;
            }
        }
    }
    else {
        applied = copyRegularFileBeneathRoot(
            context.snapshotDirFd, relativePath, entry.path, context.useReflink);
    }

    // 退避物の破棄は復元成功後にのみ行う。
    // 失敗時は元の位置へ戻し、復元できなかったlive側のデータを消さない。
    // 戻すことすらできない場合も退避物は削除せず、復旧できるようpathを記録する
    if (!detachedPath.isEmpty()) {
        if (applied) {
            if (!qsnapper::security::safeRemoveAllBeneathRoot(
                    QStringLiteral("/"), detachedPath)) {
                qWarning() << "Staged restore: Failed to remove detached live path:"
                           << strerror(errno);
            }
        }
        else if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(
                     QStringLiteral("/"), detachedPath, entry.path)) {
            qCritical() << "Staged restore: Failed to reattach live path after a failed"
                        << "restore. Previous content is preserved at:" << detachedPath;
        }
    }

    return applied;
}

/**
 * @brief live宛先をroot配下で再解決して通常ファイルをコピーする
 *
 * live側を直接O_TRUNCで開くと、実行中のバイナリ (復元を実行しているqSnapper自身や
 * 稼働中のサービス) がETXTBSYで拒否されるため、同一ディレクトリの一時ファイルへ
 * 書き出してから renameat() で差し替える (copySymlinkBeneathRootと同じ手法)。
 * metadataは差し替え前に一時ファイルへ適用するため、live側から不完全な状態は観測されない
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param dst live filesystem上の絶対path
 * @param tryReflink FICLONEを先行試行するか
 * @return dataと必須metadataを適用して差し替えできた場合true
 */
bool SnapshotOperations::copyRegularFileBeneathRoot(int sourceDirFd,
                                                    const QString &sourceRelativePath,
                                                    const QString &dst,
                                                    bool tryReflink)
{
    const int srcFd = qsnapper::security::safeOpenRegularFileReadAt(
        sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to open source:"
                   << sourceRelativePath << strerror(errno);
        return false;
    }

    struct stat srcStat;
    if (::fstat(srcFd, &srcStat) < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to stat source:"
                   << strerror(errno);
        ::close(srcFd);
        return false;
    }

    QString temporaryPath;
    int dstFd = -1;
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporaryPath = siblingTemporaryPath(dst, QStringLiteral("qsnapper-copy"),
                                             attempt);
        dstFd = qsnapper::security::safeCreateRegularFileExclusiveBeneathRoot(
            QStringLiteral("/"), temporaryPath, srcStat.st_mode & 07777);
        if (dstFd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            qWarning() << "copyRegularFileBeneathRoot: Failed to create temporary file:"
                       << strerror(errno);
            ::close(srcFd);
            return false;
        }
    }

    if (dstFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to allocate temporary file path";
        ::close(srcFd);
        return false;
    }

    // 差し替え前に失敗した場合、live側へ中途半端な一時ファイルを残さない
    const auto abortCopy = [&]() {
        ::close(dstFd);
        ::close(srcFd);
        qsnapper::security::safeRemoveAllBeneathRoot(QStringLiteral("/"),
                                                     temporaryPath);
        return false;
    };

    bool copied = false;
    if (tryReflink && ::ioctl(dstFd, FICLONE, srcFd) == 0) {
        copied = true;
    }

    if (!copied) {
        off_t offset = 0;
        ssize_t remaining = srcStat.st_size;
        while (remaining > 0) {
            const ssize_t written = ::sendfile(dstFd, srcFd, &offset,
                                               remaining);
            if (written < 0) {
                qWarning() << "copyRegularFileBeneathRoot: sendfile failed:"
                           << strerror(errno);
                return abortCopy();
            }
            if (written == 0) {
                qWarning() << "copyRegularFileBeneathRoot: sendfile reached EOF before expected byte count";
                return abortCopy();
            }
            remaining -= written;
        }
    }

    // O_CREAT時のmodeはumaskで削られるため、snapshot側のmodeを明示的に適用する
    if (::fchmod(dstFd, srcStat.st_mode & 07777) < 0) {
        qWarning() << "copyRegularFileBeneathRoot: fchmod failed"
                   << strerror(errno);
        return abortCopy();
    }

    const bool mustPreserveMetadata = (::geteuid() == 0);
    if (::fchown(dstFd, srcStat.st_uid, srcStat.st_gid) < 0) {
        qWarning() << "copyRegularFileBeneathRoot: fchown failed"
                   << strerror(errno);
        if (mustPreserveMetadata) {
            return abortCopy();
        }
    }

    // 新規inodeへ差し替えるため、live側の既存labelは引き継がれない。
    // snapshot側のlabelを明示的にコピーする
    copySecurityContextBestEffort(srcFd, dstFd);

    struct timespec times[2];
    times[0] = srcStat.st_atim;
    times[1] = srcStat.st_mtim;
    if (::futimens(dstFd, times) < 0) {
        qWarning() << "copyRegularFileBeneathRoot: futimens failed"
                   << strerror(errno);
        if (mustPreserveMetadata) {
            return abortCopy();
        }
    }

    ::close(dstFd);
    ::close(srcFd);

    if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(
            QStringLiteral("/"), temporaryPath, dst)) {
        const int renameErrno = errno;
        qsnapper::security::safeRemoveAllBeneathRoot(QStringLiteral("/"),
                                                     temporaryPath);
        qWarning() << "copyRegularFileBeneathRoot: rename failed:"
                   << strerror(renameErrno);
        return false;
    }

    return true;
}

/**
 * @brief live宛先をroot配下で再解決してsymlinkをコピーする
 * @param src snapshot内の読み取り元path
 * @param dst live filesystem上の絶対path
 * @return symlinkを作成できた場合true
 */
bool SnapshotOperations::copySymlinkBeneathRoot(int sourceDirFd,
                                                const QString &sourceRelativePath,
                                                const QString &dst)
{
    QByteArray linkTarget;
    if (!qsnapper::security::safeReadLinkNoFollowAt(sourceDirFd, sourceRelativePath,
                                                    &linkTarget)) {
        qWarning() << "copySymlinkBeneathRoot: readlink failed:"
                   << sourceRelativePath << strerror(errno);
        return false;
    }

    QString temporaryPath;
    bool createdTemporaryLink = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporaryPath = siblingTemporaryPath(
            dst, QStringLiteral("qsnapper-link"), attempt);
        if (qsnapper::security::safeCreateSymlinkNoFollowBeneathRoot(
                QStringLiteral("/"), linkTarget, temporaryPath)) {
            createdTemporaryLink = true;
            break;
        }
        if (errno != EEXIST) {
            qWarning() << "copySymlinkBeneathRoot: symlink(temp) failed:"
                       << strerror(errno);
            return false;
        }
    }

    if (!createdTemporaryLink) {
        qWarning() << "copySymlinkBeneathRoot: Failed to allocate temporary link path";
        return false;
    }

    if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(
            QStringLiteral("/"), temporaryPath, dst)) {
        const int renameErrno = errno;
        qsnapper::security::safeRemoveAllBeneathRoot(
            QStringLiteral("/"), temporaryPath);
        qWarning() << "copySymlinkBeneathRoot: rename failed:"
                   << strerror(renameErrno);
        return false;
    }

    struct stat srcStat;
    if (qsnapper::security::safeLstatAt(sourceDirFd, sourceRelativePath, &srcStat)) {
        struct timespec times[2];
        times[0] = srcStat.st_atim;
        times[1] = srcStat.st_mtim;

        bool ownerUpdated = false;
        bool timesUpdated = false;
        if (!qsnapper::security::safeSetSymlinkMetadataNoFollowBeneathRoot(
                QStringLiteral("/"), dst, srcStat.st_uid, srcStat.st_gid,
                times, &ownerUpdated, &timesUpdated)) {
            if (!ownerUpdated) {
                qWarning() << "copySymlinkBeneathRoot: fchownat failed (non-fatal):"
                           << strerror(errno);
            }
            if (!timesUpdated) {
                qWarning() << "copySymlinkBeneathRoot: utimensat failed (non-fatal):"
                           << strerror(errno);
            }
        }
    }

    return true;
}
