#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QDebug>
#include <QPointer>

// glibのGDBusInterfaceInfo / GDBusObjectSkeletonは"signals"というメンバ名を持ち、Qtのsignalsマクロと衝突する
// そのため、polkitヘッダの取り込み中だけマクロを外す
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
#include <sys/statvfs.h>
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
#include "csvrecord.h"
#include "restorevalidation.h"

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
// polkit-qt6-1の非同期API (Authority::checkAuthorization + checkAuthorizationFinished) は使用しない
// Authorityはシングルトンであり、完了シグナルはResult値しか運ばず、完了callbackのuser_dataもAuthority自身であるため、
// 同時に2件の認可が進行すると、どのD-Bus呼び出しに対する結果なのかを判別できない (rootサービスでは「別要求の許可」を流用してしまう認可の取り違えに直結する)
// またcancellableとエラー状態がシングルトンで共有されており、1件の失敗が後続の全呼び出しを黙って落とす
//
// polkitのGObject APIは呼び出しごとにGSimpleAsyncResultとuser_dataを確保するため、完了callbackを発行元の呼び出しへ厳密に対応付けられる
// 完了callbackは呼び出し時点のthread-default GMainContext (=Qtのイベントループが回すdefault context) で発火する
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
     * user_dataは本呼び出し専用に確保したAsyncAuthorizationであり、他の認可要求の結果と混ざらない
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

bool qsnapper::restore::isPinnedRestoreSourceReadOnly(int snapshotDirFd, const std::string &fstype)
{
    if (fstype == "btrfs") {
        bool readOnly = false;
        return btrfs_util_get_subvolume_read_only_fd(snapshotDirFd, &readOnly) == BTRFS_UTIL_OK
               && readOnly;
    }

    struct statvfs fsInfo = {};
    return ::fstatvfs(snapshotDirFd, &fsInfo) == 0 && (fsInfo.f_flag & ST_RDONLY) != 0;
}

/**
 * @brief 上限を指定して構築する
 * @param perUidLimit 1つのUIDが同時に保持できる件数の上限
 * @param globalLimit 全体で同時に保持できる件数の上限
 */
qsnapper::security::PendingAuthorizationBudget::PendingAuthorizationBudget(int perUidLimit, int globalLimit)
    : m_perUidLimit(perUidLimit)
    , m_globalLimit(globalLimit)
{
}

/**
 * @brief 1件分の枠を確保する
 * @param uid 呼び出し元のUID
 * @return UID単位と全体の両方の上限内であれば確保してtrue、それ以外は何もせずfalse
 */
bool qsnapper::security::PendingAuthorizationBudget::tryAcquire(uint uid)
{
    if (m_total >= m_globalLimit || m_perUid.value(uid, 0) >= m_perUidLimit) {
        return false;
    }
    ++m_perUid[uid];
    ++m_total;
    return true;
}

/**
 * @brief tryAcquire()で確保した1件分の枠を返す
 * @param uid tryAcquire()に渡したUID
 */
void qsnapper::security::PendingAuthorizationBudget::release(uint uid)
{
    const auto it = m_perUid.find(uid);
    if (it == m_perUid.end()) {
        return;
    }
    if (--it.value() <= 0) {
        m_perUid.erase(it);
    }
    if (m_total > 0) {
        --m_total;
    }
}

/**
 * @brief 全体の保持件数を返す
 * @return 保持件数
 */
int qsnapper::security::PendingAuthorizationBudget::total() const
{
    return m_total;
}

/**
 * @brief 指定したUIDの保持件数を返す
 * @param uid 呼び出し元のUID
 * @return 保持件数
 */
int qsnapper::security::PendingAuthorizationBudget::countFor(uint uid) const
{
    return m_perUid.value(uid, 0);
}

namespace {

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

        // leaf名はNAME_MAX (bytes) を超えられない
        // 長いbase名はここで切り詰めるが、suffixのpid / ms / attemptにより一意性は保たれる
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

} // anonymous namespace

namespace qsnapper::diff {

    namespace {

        /**
         * @brief Myers diffの編集操作
         */
        struct DiffOp {
            enum Type { Equal, Delete, Insert };
            Type type;
            int aIdx, bIdx;  // 0-based index into old/new lines (-1 if N/A)
        };

        /**
         * @brief diff入力の読み込み結果
         */
        enum class DiffInputStatus {
            Ok,             // 読み込み成功
            NotRegular,     // ディレクトリやsymlink (差分の対象外)
            SpecialFile,    // FIFO・デバイス・ソケット (開かない)
            TooLarge,       // サイズまたは行数が上限を超える
            Binary,         // 先頭にNULを含む
            Unreadable      // 読み込めない
        };

        /**
         * @brief diff入力のファイルを上限付きで読み込む
         *
         * 開く前にlstatで種別とサイズを確認し、FIFOやデバイスは開かない
         * 読み込み中にファイルが伸びても、上限 + 1バイトまでしか読まない
         *
         * @param path 対象パス
         * @param limits 資源上限
         * @param contentOut 読み込んだ内容 ('\r'は除去済み)
         * @return 読み込み結果
         */
        DiffInputStatus readDiffInput(const QString &path, const UnifiedDiffLimits &limits,
                                      QByteArray *contentOut)
        {
            struct stat st;
            if (!qsnapper::security::safeLstat(path, &st)) {
                return DiffInputStatus::Unreadable;
            }
            if (S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)
                    || S_ISSOCK(st.st_mode)) {
                return DiffInputStatus::SpecialFile;
            }
            if (!S_ISREG(st.st_mode)) {
                return DiffInputStatus::NotRegular;
            }
            if (st.st_size > limits.maxFileBytes) {
                return DiffInputStatus::TooLarge;
            }

            const int fd = qsnapper::security::safeOpenRegularFileRead(path);
            if (fd < 0) {
                return DiffInputStatus::Unreadable;
            }

            QByteArray data;
            const qint64 readLimit = limits.maxFileBytes + 1;
            data.resize(static_cast<qsizetype>(qMin<qint64>(qMax<qint64>(st.st_size, 0) + 1, readLimit)));
            qint64 total = 0;
            bool readFailed = false;
            while (total < readLimit) {
                if (total == data.size()) {
                    data.resize(static_cast<qsizetype>(qMin<qint64>(qMax<qint64>(total * 2, 4096), readLimit)));
                }
                const ssize_t n = ::read(fd, data.data() + total, static_cast<size_t>(data.size() - total));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    readFailed = true;
                    break;
                }
                if (n == 0) {
                    break;
                }
                total += n;
            }
            ::close(fd);

            if (readFailed) {
                return DiffInputStatus::Unreadable;
            }
            if (total > limits.maxFileBytes) {
                return DiffInputStatus::TooLarge;
            }
            data.truncate(static_cast<qsizetype>(total));

            const qsizetype probeLength = static_cast<qsizetype>(qMin<qint64>(total, limits.binaryProbeBytes));
            if (QByteArrayView(data.constData(), probeLength).contains('\0')) {
                return DiffInputStatus::Binary;
            }
            if (data.count('\n') + 1 > limits.maxLinesPerFile) {
                return DiffInputStatus::TooLarge;
            }

            // QIODevice::Textでの読み込みと同じく'\r'を除去する
            data.replace("\r", "");
            *contentOut = data;
            return DiffInputStatus::Ok;
        }

        /**
         * Myers diffアルゴリズムで2つの行ID列間の最短編集スクリプトを計算する
         *
         * 各dステップ開始時のVのうち、バックトラックで参照する範囲 [-d, d] だけをtraceに保存する (合計 (D + 1)^2 要素)
         * V配列とtraceの合計サイズ、および探索ステップ数が上限を超えた場合は計算を打ち切る
         *
         * @param a 旧ファイルの行ID列
         * @param b 新ファイルの行ID列
         * @param limits 資源上限
         * @param opsOut 編集操作のリスト (正順)
         * @return 上限内で計算できた場合true
         */
        bool computeMyersDiff(const QVector<int> &a, const QVector<int> &b,
                              const UnifiedDiffLimits &limits, QVector<DiffOp> *opsOut)
        {
            const qsizetype N = a.size(), M = b.size();
            QVector<DiffOp> &result = *opsOut;
            result.clear();

            if (N == 0 && M == 0) return true;
            if (N == 0) {
                result.reserve(M);
                for (qsizetype i = 0; i < M; i++)
                    result.append({DiffOp::Insert, -1, static_cast<int>(i)});
                return true;
            }
            if (M == 0) {
                result.reserve(N);
                for (qsizetype i = 0; i < N; i++)
                    result.append({DiffOp::Delete, static_cast<int>(i), -1});
                return true;
            }

            const qsizetype MAX = N + M, OFF = MAX;
            const qint64 vBytes = static_cast<qint64>(2 * MAX + 1) * static_cast<qint64>(sizeof(int));
            if (vBytes > limits.maxTraceBytes) {
                return false;
            }

            // V[k + OFF] = 対角線k上の最遠到達x座標
            QVector<int> V(2 * MAX + 1, 0);

            // dステップ開始前 (= d-1ステップ終了後) のV[-d..d] (offset d*d、長さ2d+1)
            QVector<int> trace;
            qint64 steps = 0;
            qsizetype finalD = -1;

            for (qsizetype d = 0; d <= MAX && finalD < 0; d++) {
                const qint64 traceBytes = static_cast<qint64>(trace.size() + 2 * d + 1)
                                          * static_cast<qint64>(sizeof(int));
                if (vBytes + traceBytes > limits.maxTraceBytes) {
                    return false;
                }
                for (qsizetype k = -d; k <= d; k++) {
                    trace.append(V[OFF + k]);
                }

                for (qsizetype k = -d; k <= d; k += 2) {
                    if (++steps > limits.maxSteps) {
                        return false;
                    }
                    qsizetype x = (k == -d || (k != d && V[OFF + k - 1] < V[OFF + k + 1]))
                                  ? V[OFF + k + 1] : V[OFF + k - 1] + 1;
                    qsizetype y = x - k;
                    while (x < N && y < M && a[x] == b[y]) {
                        x++; y++;
                        if (++steps > limits.maxSteps) {
                            return false;
                        }
                    }
                    V[OFF + k] = static_cast<int>(x);
                    if (x >= N && y >= M) {
                        finalD = d;
                        break;
                    }
                }
            }

            // バックトラックで編集スクリプトを逆順に構築
            QVector<DiffOp::Type> revTypes;
            revTypes.reserve(N + M);
            qsizetype x = N, y = M;

            for (qsizetype d = finalD; d > 0; d--) {
                const int *vp = trace.constData() + d * d + d;  // vp[k] = d-1ステップ終了後のV[k]
                const qsizetype k = x - y;
                const bool down = (k == -d) || (k != d && vp[k - 1] < vp[k + 1]);
                const qsizetype pk = down ? k + 1 : k - 1;
                const qsizetype px = vp[pk], py = px - pk;
                const qsizetype mx = down ? px : px + 1, my = mx - k;

                // 対角線上の等号行 (snake) を逆順に記録
                while (x > mx && y > my) {
                    x--; y--;
                    revTypes.append(DiffOp::Equal);
                }

                // 非対角移動 (挿入/削除)
                revTypes.append(down ? DiffOp::Insert : DiffOp::Delete);
                x = px; y = py;
            }

            // d=0の初期snake (等号行のみ、編集なし)
            while (x > 0 && y > 0) {
                x--; y--;
                revTypes.append(DiffOp::Equal);
            }

            // 正順に反転
            std::reverse(revTypes.begin(), revTypes.end());

            // 操作タイプからインデックス付きDiffOpに変換
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
            return true;
        }

        /**
         * @brief 内容を行に分割する (末尾の改行で生じる空要素は除去する)
         * @param content ファイル内容
         * @return 行のリスト
         */
        QStringList splitDiffLines(const QByteArray &content)
        {
            QStringList lines = QString::fromUtf8(content).split('\n');
            if (!lines.isEmpty() && lines.last().isEmpty()) {
                lines.removeLast();
            }
            return lines;
        }

    }

    UnifiedDiffResult generateUnifiedDiff(const QString &oldPath, const QString &newPath,
                                          const UnifiedDiffLimits &limits)
    {
        try {
            QByteArray oldContent;
            QByteArray newContent;
            const DiffInputStatus oldStatus = readDiffInput(oldPath, limits, &oldContent);
            const DiffInputStatus newStatus = readDiffInput(newPath, limits, &newContent);

            const auto either = [&](DiffInputStatus status) {
                return oldStatus == status || newStatus == status;
            };
            if (either(DiffInputStatus::SpecialFile)) {
                return {QString(), QStringLiteral("special_file")};
            }
            if (either(DiffInputStatus::TooLarge)) {
                return {QString(), QStringLiteral("too_large")};
            }
            if (either(DiffInputStatus::Binary)) {
                return {QString(), QStringLiteral("binary")};
            }
            if (oldStatus != DiffInputStatus::Ok || newStatus != DiffInputStatus::Ok) {
                return {};
            }

            const QStringList a = splitDiffLines(oldContent);
            const QStringList b = splitDiffLines(newContent);

            // 行を整数IDへ置き換え、Myers diffの比較を整数比較にする
            QHash<QString, int> lineIds;
            const auto toIds = [&lineIds](const QStringList &lines) {
                QVector<int> ids;
                ids.reserve(lines.size());
                for (const QString &line : lines) {
                    auto it = lineIds.constFind(line);
                    if (it == lineIds.cend()) {
                        it = lineIds.insert(line, static_cast<int>(lineIds.size()));
                    }
                    ids.append(it.value());
                }
                return ids;
            };
            const QVector<int> aIds = toIds(a);
            const QVector<int> bIds = toIds(b);

            QVector<DiffOp> ops;
            if (!computeMyersDiff(aIds, bIds, limits, &ops)) {
                return {QString(), QStringLiteral("too_many_changes")};
            }

            // 変更がない場合は空文字列を返す (diff -u の差分なしと同じ挙動)
            const int context = 3;
            QVector<int> changes;
            for (int i = 0; i < ops.size(); i++) {
                if (ops[i].type != DiffOp::Equal) changes.append(i);
            }
            if (changes.isEmpty()) return {};

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
                int e = qMin(static_cast<int>(ops.size()) - 1, h.end + context);

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

            return {out, QString()};
        }
        catch (const std::bad_alloc &) {
            return {QString(), QStringLiteral("too_large")};
        }
    }

}

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
            Q_UNUSED(path);
            resetIdleTimer();
            emit restorePlanProgress(manifestId, current, total);
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

    m_rootReadOnlyProbe = [](bool *readOnly) {
        return btrfs_util_get_subvolume_read_only("/", readOnly) == BTRFS_UTIL_OK;
    };
    m_rootReadWriteRestorer = []() {
        const auto result = btrfs_util_set_subvolume_read_only("/", false);
        if (result != BTRFS_UTIL_OK) {
            qCritical() << "Staged restore: Failed to restore root subvolume rw state:"
                        << result;
            return false;
        }
        return true;
    };

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
 * (プロンプトはタイムアウトを持たないため、認証中にアイドル終了してしまうと、ユーザがパスワードを入力した直後に呼び出しが失われる)
 */
void SnapshotOperations::resetIdleTimer()
{
    if (m_pendingAuthorizations.total() > 0) {
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
 * @brief マニフェスト状態をD-Bus contractの小文字表現へ変換する
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
 * @brief restorePlanFinished signalに載せる終端状態ごとの固定文言を返す
 * @param state 終端状態
 * @return pathなどの詳細を含まない固定文言
 */
QString SnapshotOperations::restorePlanFinishedSignalMessage(
    qsnapper::restore::ManifestState state)
{
    using qsnapper::restore::ManifestState;

    switch (state) {
    case ManifestState::Completed:
        return QStringLiteral("Restore completed");
    case ManifestState::Cancelled:
        return QStringLiteral("Restore cancelled");
    case ManifestState::Staging:
    case ManifestState::Frozen:
    case ManifestState::Running:
    case ManifestState::Failed:
        break;
    }
    return QStringLiteral("Restore failed");
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
 * @brief RFC4180形式で必要なCSVフィールドをクォートする
 * @param フィールドクォート対象文字列
 * @return CSVへ安全に埋め込めるフィールド
 *
 * 実装はqsnapper::csv::quoteField() (include/csvrecord.h) へ委譲する
 * スナップショット一覧CSVと同じ実装を使うことでproducer/consumerの対称性を保証する
 * 共有実装はCR / LFもクォート対象とする (旧実装の上位互換)
 *
 * クォート対象のconfigName / lastErrorは、それぞれ設定名検証とqSnapper内部生成のメッセージに由来し実際にはCR / LFを含まないため、出力は従来と同一である
 */
QString SnapshotOperations::quoteRestoreStatusCsvField(const QString &field)
{
    return qsnapper::csv::quoteField(field);
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
 * @brief 指定マニフェストのmountを解除して実行contextを削除する
 * @param manifestId cleanup対象マニフェストID
 */
void SnapshotOperations::cleanupRestoreExecution(const QString &manifestId)
{
    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution == m_restoreExecutions.end()) {
        return;
    }

    // dirfdを先に閉じてからunmountを要求する
    // 開いたfdがmount点をbusyのままにすると、検証失敗時の後片付けが
    // mount leakに化けるため、順序を逆にしてはならない
    if (execution->snapshotDirFd >= 0) {
        ::close(execution->snapshotDirFd);
        execution->snapshotDirFd = -1;
    }
    const RestoreExecution context = execution.value();
    unmountRestoreExecution(context);
    m_restoreExecutions.erase(execution);
}

/**
 * @brief rootサブボリュームのread-only状態の取得とrw化の処理をテスト用に差し替える
 * @param readOnlyProbe read-only状態を取得する関数 (取得できた場合true)
 * @param readWriteRestorer rwへ戻す関数 (成功した場合true)
 */
void SnapshotOperations::setRootSubvolumeAccessForTesting(
    std::function<bool(bool *)> readOnlyProbe,
    std::function<bool()> readWriteRestorer)
{
    m_rootReadOnlyProbe = std::move(readOnlyProbe);
    m_rootReadWriteRestorer = std::move(readWriteRestorer);
}

/**
 * @brief 実行を開始した復元計画に限り、rootサブボリュームをrwへ戻す安全ネットを実行する
 * @param state commit時に記録した安全ネットの判定材料
 */
void SnapshotOperations::runRootReadWriteSafetyNet(
    const qsnapper::restore::RootReadWriteSafetyNetState &state)
{
    // 復元後にread-onlyだったと仮定しても条件を満たさない計画では、状態の取得すら行わない
    // 未認可のBegin -> Cancelなどから特権操作へ到達させないため
    if (!qsnapper::restore::shouldRestoreRootReadWrite(state, true)) {
        return;
    }

    bool readOnly = false;
    if (!m_rootReadOnlyProbe || !m_rootReadOnlyProbe(&readOnly)) {
        qWarning() << "Staged restore: Failed to read root subvolume read-only state";
        return;
    }
    if (!qsnapper::restore::shouldRestoreRootReadWrite(state, readOnly)) {
        return;
    }

    qWarning() << "Staged restore: Root subvolume became read-only after restore, restoring rw";
    if (m_rootReadWriteRestorer) {
        m_rootReadWriteRestorer();
    }
}

/**
 * @brief 終端計画の安全ネット・unmount・signal・registry削除を実行する
 * @param manifestId 終端したマニフェストID
 * @param terminal 終端状態
 * @param messageText 終端理由
 */
void SnapshotOperations::finishRestorePlan(
    const QString &manifestId,
    qsnapper::restore::ManifestState terminal,
    const QString &messageText)
{
    // 実行contextは認可済みcommitでのみ作られるため、未認可で終端した計画では安全ネットもunmountも走らない
    const auto execution = m_restoreExecutions.find(manifestId);
    if (execution != m_restoreExecutions.end()) {
        runRootReadWriteSafetyNet(execution->rootReadWriteState);

        // cleanupRestoreExecutionと同じ順序規約: dirfdを先に閉じる
        if (execution->snapshotDirFd >= 0) {
            ::close(execution->snapshotDirFd);
            execution->snapshotDirFd = -1;
        }
        unmountRestoreExecution(execution.value());
    }

    // 詳細 (失敗したpathを含み得る) はownerだけが取得できるように残し、signalには固定文言だけを載せる
    const QString owner = m_restorePlanOwners.value(manifestId);
    qsnapper::restore::ManifestStatus finishedStatus =
        m_restoreRegistry.status(manifestId, owner, nullptr)
            .value_or(qsnapper::restore::ManifestStatus{});
    finishedStatus.id = manifestId;
    finishedStatus.state = terminal;
    if (finishedStatus.lastError.isEmpty()) {
        finishedStatus.lastError = messageText;
    }
    m_finishedRestorePlans.record(owner, finishedStatus);

    emit restorePlanFinished(manifestId,
                             restoreManifestStateString(terminal),
                             restorePlanFinishedSignalMessage(terminal));

    m_restoreExecutions.remove(manifestId);
    m_restoreRegistry.remove(manifestId);
    m_restorePlanOwners.remove(manifestId);
    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief owner消失時に予約済み実行とmountとマニフェストを全て破棄する
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
    m_finishedRestorePlans.removeByOwner(owner);
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
    m_finishedRestorePlans.purgeExpired();

    const QStringList planIds = m_restorePlanOwners.keys();
    for (const QString &manifestId : planIds) {
        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        const QString owner = m_restorePlanOwners.value(manifestId);
        if (!m_restoreRegistry.status(manifestId, owner, &error)) {
            // executorが同じ計画をもう1度終端しないよう先にabandonしてから、finishRestorePlanで終端する
            // ここでcleanupRestoreExecutionだけを呼ぶとrestorePlanFinishedが発火せず、
            // クライアントは完了通知を永久に待ち続け、復元でread-onlyになったrootサブボリュームをrwへ戻す安全ネットも実行されない
            m_restoreExecutor.abandon(manifestId);
            finishRestorePlan(
                manifestId, qsnapper::restore::ManifestState::Failed,
                QStringLiteral("Restore plan expired before completion"));
        }
    }

    removeUnusedRestoreOwnerWatches();
}

/**
 * @brief マニフェストを持たないownerをservice watcherから除外する
 */
void SnapshotOperations::removeUnusedRestoreOwnerWatches()
{
    if (!m_ownerWatcher) {
        return;
    }

    const QStringList watchedOwners = m_ownerWatcher->watchedServices();
    for (const QString &owner : watchedOwners) {
        // 終端した計画も保持期間中はownerの切断時に掃除する必要があるため、watchを残す
        if (m_restoreRegistry.countForOwner(owner) == 0
                && m_finishedRestorePlans.countForOwner(owner) == 0) {
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

    // 許可リスト外のキーや不正な値は認可前に拒否する (キーと値はエラー本文に含めない)
    if (!qsnapper::security::validateSnapperConfigSettings(settings)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Invalid config settings"));
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
        m_snapshotListFingerprint.clear();

        std::map<std::string, std::string> info;
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info[it.key().toStdString()] = it.value().toStdString();
        }

        snapper->setConfigInfo(info);
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to write snapper config:" << e.what();
        replyError(QDBusError::Failed, QStringLiteral("Failed to write config"));
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
 * @param callerUid 認可待ちを開始したときの呼び出し元UID
 */
void SnapshotOperations::endPendingAuthorization(uint callerUid)
{
    m_pendingAuthorizations.release(callerUid);
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

    // 認可待ちの枠は呼び出し元のUID単位で数える
    // UIDはBeginRestorePlanと同じく、バスデーモンが保持する接続の資格情報から取得する
    // 取得できない場合は誰の枠か決められないため拒否する
    const QDBusConnectionInterface *bus = reply.connection.interface();
    const QDBusReply<uint> uidReply = bus ? bus->serviceUid(reply.message.service()) : QDBusReply<uint>();
    if (!uidReply.isValid()) {
        g_object_unref(subject);
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }
    const uint callerUid = uidReply.value();

    if (!m_pendingAuthorizations.tryAcquire(callerUid)) {
        g_object_unref(subject);
        replyError(QDBusError::LimitsExceeded,
                   QStringLiteral("Too many pending authorization requests"));
        return AuthorizationOutcome::Denied;
    }

    // 認可要求ごとに固有のuser_dataを渡し、完了結果が発行元の呼び出しへ
    // 一意に紐づくようにする
    auto *request = new AsyncAuthorization;
    request->service = this;
    request->continuation = [this, reply, continuation, callerUid](bool granted) {
        endPendingAuthorization(callerUid);
        continuation(reply, granted);
    };

    polkit_authority_check_authorization(
        authority, subject, actionId.toUtf8().constData(), nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, nullptr,
        asyncAuthorizationFinished, request);

    // polkit_authority_check_authorization()はsubjectを同期的にGVariant化するため、呼び出し直後に解放してよい
    g_object_unref(subject);

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
            // 指紋は再生成前に取ったものだけが有効であるため、ここで破棄する (getSnapperForListing()が設定し直す)
            m_snapshotListFingerprint.clear();
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
 * @brief 一覧の取得用にSnapperインスタンスを取得する
 *
 * libsnapperのSnapperは構築時に一覧を読み込み、外部で作成・削除・変更されたスナップショットを取り込まない
 * そのため、.snapshots の指紋が読み込み時から変わっていれば再生成する
 *
 * 一覧を無効化する条件:
 *   - 設定の切り替え・強制再生成 (getSnapper()が指紋を破棄する)
 *   - 本サービスによる作成・削除・変更・rollback・設定変更 (各処理が変更前に指紋を破棄する)
 *   - 外部による作成・削除・変更 (指紋の不一致として検出する)
 *   - 指紋を計算できない場合
 *
 * @param configName 検証済みSnapper設定名
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 */
snapper::Snapper* SnapshotOperations::getSnapperForListing(const QString &configName)
{
    // 指紋は再生成より先に取る
    // 再生成の途中で一覧が変わった場合も、保存した指紋とは一致しなくなるため、次回に再生成される
    QByteArray fingerprint;
    try {
        const snapper::ConfigInfo configInfo = snapper::Snapper::getConfig(configName.toStdString(), "/");
        QString snapshotsDir = QString::fromStdString(configInfo.get_subvolume());
        if (!snapshotsDir.endsWith(QLatin1Char('/'))) {
            snapshotsDir += QLatin1Char('/');
        }
        snapshotsDir += QStringLiteral(".snapshots");
        fingerprint = qsnapper::security::snapshotListFingerprint(snapshotsDir);
    }
    catch (const snapper::Exception &) {
        // e.what()には設定ファイルのpath (指定された設定名) が含まれ得るため、ログには載せない
        qWarning() << "Failed to read Snapper config for listing";
    }

    if (m_snapper && m_currentConfig == configName && !fingerprint.isEmpty()
            && fingerprint == m_snapshotListFingerprint) {
        return m_snapper.get();
    }

    snapper::Snapper *snapper = getSnapper(configName, /*forceReload=*/true);
    if (snapper) {
        m_snapshotListFingerprint = fingerprint;
    }
    return snapper;
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

        // 本CSVは、"1行 = 1スナップショット"である
        // description / cleanup / userdataは、snapper CLIやpluginからも書き込まれる信頼できない値であり、
        // 改行を混入されると行が割れて攻撃者の選んだnumberを持つ偽のスナップショット行が生まれる
        // 偽のnumberはクライアントからDeleteSnapshot / RollbackSnapshotへそのまま渡る
        //
        // これらは表示用の値であるため、制御文字をU+FFFDに置き換えて出力する
        // 1件の不正な値で一覧全体を失敗させず、かつ行を割らない (numberはlibsnapperの値であり、置き換えの影響を受けない)
        // カンマは置き換えない (正当なdescriptionにも現れ得る)
        // 代わりにqsnapper::csv::quoteField()によるRFC 4180準拠のクォートで列ずれを防ぐ
        //
        // クォートは表示正しさのための修正であり、sanitizeRecordText()による制御文字の置き換えの上に重ねる層であって代替ではない
        const QString rawCleanup     = QString::fromStdString(snapshot.getCleanup());
        const QString rawDescription = QString::fromStdString(snapshot.getDescription());
        bool recordSafe = qsnapper::security::isRecordSafeText(rawCleanup)
                && qsnapper::security::isRecordSafeText(rawDescription);
        const QString cleanup     = qsnapper::security::sanitizeRecordText(rawCleanup);
        const QString description = qsnapper::security::sanitizeRecordText(rawDescription);

        // ユーザデータを key1=value1,key2=value2形式に変換
        // 各ペアは個別のCSVフィールドであるため、ペア文字列を組み立てた上で1フィールド毎にクォートする
        // (keyやvalueにカンマがあってもペアが2フィールドに割れない)
        const std::map<std::string, std::string> &userdata = snapshot.getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            const QString key   = QString::fromStdString(pair.first);
            const QString value = QString::fromStdString(pair.second);
            if (!qsnapper::security::isRecordSafeText(key) || !qsnapper::security::isRecordSafeText(value)) {
                recordSafe = false;
            }
            userdataPairs.append(qsnapper::csv::quoteField(qsnapper::security::sanitizeRecordText(key) + "="
                                                           + qsnapper::security::sanitizeRecordText(value)));
        }
        if (!recordSafe) {
            // 攻撃者が制御する値はログに載せない (ログ注入防止)
            qWarning() << "Snapshot listing: replaced control characters in metadata of snapshot"
                       << snapshot.getNum();
        }

        csv += QString::number(snapshot.getNum()) + ",";
        csv += snapshotTypeToString(snapshot.getType()) + ",";
        csv += QString::number(snapshot.getPreNum()) + ",";

        // 日時をISO形式に変換
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(snapshot.getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(snapshot.getUid()) + ",";
        // cleanup / description / userdataペアはquoteField()でクォートする (前述の注記を参照)
        csv += qsnapper::csv::quoteField(cleanup) + ",";
        csv += qsnapper::csv::quoteField(description) + ",";

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
        // 外部で作成・削除・変更されたスナップショットを反映するため、一覧が変わっている場合は再構築する
        snapper::Snapper *snapper = getSnapperForListing(configName);
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

        // 格納前にレコード境界を破壊する値を拒否する理由はmodifySnapshotAuthorized()側の注記を参照すること
        if (!qsnapper::security::isRecordSafeText(description)
                || !qsnapper::security::isRecordSafeText(cleanup)) {
            replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Snapshot metadata must not contain control characters"));
            return QString();
        }
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            if (!qsnapper::security::isRecordSafeText(it.key())
                    || !qsnapper::security::isRecordSafeText(it.value())) {
                replyError(QDBusError::InvalidArgs,
                           QStringLiteral("Snapshot metadata must not contain control characters"));
                return QString();
            }
        }

        // スナップショット作成 (スナップショット一覧の変化) 前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

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

        // レコード境界を破壊する値を拒否する理由はformatSnapshotToCSV()側の注記を参照すること
        const QString cleanup     = QString::fromStdString(newSnapshot->getCleanup());
        const QString description = QString::fromStdString(newSnapshot->getDescription());

        // ユーザデータを key1=value1,key2=value2形式に変換
        // 各ペアは個別のCSVフィールドであるため、formatSnapshotToCSV()と同様にペア単位でクォートする
        const std::map<std::string, std::string> &userdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        bool recordSafe = qsnapper::security::isRecordSafeText(cleanup) && qsnapper::security::isRecordSafeText(description);
        for (const auto &pair : userdata) {
            const QString key   = QString::fromStdString(pair.first);
            const QString value = QString::fromStdString(pair.second);
            if (!qsnapper::security::isRecordSafeText(key) || !qsnapper::security::isRecordSafeText(value)) {
                recordSafe = false;
                break;
            }
            userdataPairs.append(qsnapper::csv::quoteField(key + "=" + value));
        }
        if (!recordSafe) {
            // スナップショット自体は作成済みである
            // 応答だけを拒否し、偽装可能なCSVをクライアントへ渡さない
            qWarning() << "Rejected snapshot creation reply: snapshot" << newSnapshot->getNum()
                       << "has metadata containing control characters";
            replyError(QDBusError::Failed, QStringLiteral("Snapshot metadata contains control characters"));
            return QString();
        }

        // 新しく作成されたスナップショットのCSV情報を返す
        // クォートは表示正しさのための修正であり、isRecordSafeText()による制御文字拒否の上に重ねる層であって代替ではない
        QString csv = "number,type,pre-number,date,user,cleanup,description,userdata\n";
        csv += QString::number(newSnapshot->getNum()) + ",";
        csv += snapshotTypeToString(newSnapshot->getType()) + ",";
        csv += QString::number(newSnapshot->getPreNum()) + ",";

        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(newSnapshot->getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(newSnapshot->getUid()) + ",";
        csv += qsnapper::csv::quoteField(cleanup) + ",";
        csv += qsnapper::csv::quoteField(description) + ",";

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
    // 格納前にレコード境界を破壊する値を拒否する
    // ここを通過したmetadataはListSnapshots等でそのまま連結されるため、qSnapper自身が一覧取得を不能にする値を保存しない
    // カンマは列をずらすだけでレコード境界を壊さないため対象外とする
    if (!qsnapper::security::isRecordSafeText(description) || !qsnapper::security::isRecordSafeText(cleanup)) {
        replyError(QDBusError::InvalidArgs, QStringLiteral("Snapshot metadata must not contain control characters"));
        return false;
    }
    for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
        if (!qsnapper::security::isRecordSafeText(it.key()) || !qsnapper::security::isRecordSafeText(it.value())) {
            replyError(QDBusError::InvalidArgs, QStringLiteral("Snapshot metadata must not contain control characters"));
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

        // スナップショット属性変更前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

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

    // 実行中の復元計画が復元元として参照しているスナップショットは削除しない
    // pin済みdirfdがソース読み取りの同一性を保証するが、復元中の削除は「認可時に存在した復元元」の消失につながるため、ここで明示的に拒否する
    for (const RestoreExecution &executionItem : m_restoreExecutions) {
        if (executionItem.configName == configName && executionItem.snapshotNumber == number) {
            replyError(QDBusError::Failed, QStringLiteral("Snapshot is in use by an active restore plan"));
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

        // スナップショット削除 (スナップショット一覧の変化) 前にComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

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

    // 0は現在のシステム (current) を表し、ロールバック先にはできない (snapper CLIと同じ)
    if (number <= 0) {
        replyError(QDBusError::InvalidArgs,
                   QStringLiteral("Invalid snapshot number"));
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
bool SnapshotOperations::rollbackSnapshotAuthorized(const QString &configName, int number)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        // snapper CLI (cmd-rollback) と同じく、SUBVOLUMEが "/" のbtrfs設定以外では拒否する
        // 他の設定でdefault subvolumeを切り替えると、rootファイルシステムの起動対象を壊すため
        QString normalizedSubvolume;
        std::string fstype;
        snapper->getConfigInfo().get_value("FSTYPE", fstype);
        if (!qsnapper::restore::normalizeRestoreSubvolume(
                QString::fromStdString(snapper->subvolumeDir()), &normalizedSubvolume)
                || normalizedSubvolume != QStringLiteral("/")
                || fstype != "btrfs") {
            replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Rollback is only supported for the root btrfs config"));
            return false;
        }

        snapper::Snapshots &snapshots = snapper->getSnapshots();
        snapper::Snapshots::iterator target = snapshots.find(number);
        if (target == snapshots.end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // ロールバックはスナップショット一覧・現在状態を変化させるためComparisonキャッシュと一覧の指紋を無効化
        m_comparisonCache.clear();
        m_snapshotListFingerprint.clear();

        // "sudo snapper rollback N"と同等の挙動を再現する
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

            // 本出力は「1行 = 1エントリ」の行指向テキストである
            // パスに改行を混入できると1エントリが複数行に割れ、2行目以降が攻撃者の選んだstatusとパスを持つ独立エントリとしてクライアントに解釈される
            // 検証した文字列をそのまま連結し、再取得した値を使わないこと
            const QString fileName = QString::fromStdString(file.getName());
            if (!qsnapper::security::isRecordSafeText(fileName)) {
                // 該当エントリだけを落とすと、クライアントは欠落した一覧を完全な一覧として扱ってしまうため、メソッド全体を失敗させる
                // 攻撃者が制御するパスはログにもエラー本文にも載せない (ログ注入防止)
                qWarning() << "Rejected file change listing: snapshot" << snapshotNumber
                           << "of config" << configName
                           << "contains a path with control characters";
                replyError(QDBusError::Failed, QStringLiteral("Snapshot contains a file path with control characters"));
                return QString();
            }

            output += statusStr + " " + fileName + "\n";
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

            // fail-closeする理由と、検証した文字列をそのまま連結する理由は
            // getFileChangesAuthorized()側の注記を参照すること
            const QString fileName = QString::fromStdString(file.getName());
            if (!qsnapper::security::isRecordSafeText(fileName)) {
                qWarning() << "Rejected file change listing: snapshots" << number1 << "->" << number2
                           << "of config" << configName
                           << "contain a path with control characters";
                replyError(QDBusError::Failed, QStringLiteral("Snapshot contains a file path with control characters"));
                return QString();
            }

            output += statusStr + " " + fileName + "\n";
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

        // filePathはGetFileChanges*が返したconfig相対名である
        // findAbsolutePath()はSUBVOLUME付きの絶対パスを要求し非root configで一致しないため、config相対名で引く
        auto fileIt = files.find(filePath.toStdString());
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
            const qsnapper::diff::UnifiedDiffResult diff = qsnapper::diff::generateUnifiedDiff(path1, path2);
            diffPart = diff.text;
            if (!diff.omittedReason.isEmpty()) {
                detailsPart += "diffOmitted=" + diff.omittedReason + "\n";
            }
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff between snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
    catch (const std::exception &) {
        qWarning() << "Failed to get file diff between snapshots: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Failed to get file diff"));
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

        // filePathはGetFileChanges*が返したconfig相対名である
        // findAbsolutePath()はSUBVOLUME付きの絶対パスを要求し非root configで一致しないため、config相対名で引く
        auto fileIt = files.find(filePath.toStdString());
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
            const qsnapper::diff::UnifiedDiffResult diff =
                qsnapper::diff::generateUnifiedDiff(snapshotPath, currentPath);
            diffPart = diff.text;
            if (!diff.omittedReason.isEmpty()) {
                detailsPart += "diffOmitted=" + diff.omittedReason + "\n";
            }
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff and details:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff and details: %1").arg(e.what()));
        return QString();
    }
    catch (const std::exception &) {
        qWarning() << "Failed to get file diff and details: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Failed to get file diff and details"));
        return QString();
    }
}

/**
 * @brief ownerに束縛された空のstaged restore計画を開始する
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元snapshot番号
 * @param counterpartSnapshotNumber 比較相手のsnapshot番号 (0は現在のシステム)
 * @param restoreMode yastまたはdirect
 * @return 成功時マニフェストID
 */
QString SnapshotOperations::BeginRestorePlan(const QString &configName,
                                             int snapshotNumber,
                                             int counterpartSnapshotNumber,
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

    // 0は「現在のシステム」のsentinelである (snapshot vs currentのbulk経路が使用する)
    // 正の値は復元元と別のsnapshotでなければならない
    if (counterpartSnapshotNumber < 0
            || (counterpartSnapshotNumber > 0
                && counterpartSnapshotNumber == snapshotNumber)) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid counterpart snapshot number"));
        return {};
    }

    // 計画の予算はUID単位でも数えるため、呼び出し元のUIDをバスに問い合わせる
    // 取得できない呼び出し元には計画を作らせない
    uint ownerUid = 0;
    if (calledFromDBus()) {
        const QDBusConnectionInterface *bus = connection().interface();
        const QDBusReply<uint> uidReply =
            bus ? bus->serviceUid(owner) : QDBusReply<uint>();
        if (!uidReply.isValid()) {
            replyError(QDBusError::AccessDenied,
                           QStringLiteral("Restore plan caller is unavailable"));
            return {};
        }
        ownerUid = uidReply.value();
    }
    else {
        // D-Busを介さない直接呼び出し (単体テスト) は、自プロセスの実効UIDとして数える
        ownerUid = static_cast<uint>(::geteuid());
    }

    purgeExpiredRestorePlans();

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const QString manifestId = m_restoreRegistry.createStaging(
        owner, ownerUid, *cfg, snapshotNumber, counterpartSnapshotNumber, mode,
        &error);
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
 * @param manifestId owner束縛されたマニフェストID
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

    // stagingは認可を要さないため、グローバル予算を消費する唯一の経路でもある
    // 失効済み計画をここで回収しておかないと、放置された計画が予算を占有し続け、正規の利用者がGlobalLimitで弾かれる
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
        // pathはconfig相対名であるため、SUBVOLUMEに依存しない"/"を仮の基準として形式だけを検証する
        // /.snapshotsの判定は正規化後の先頭成分で行う ("//.snapshots/..."等の迂回を防ぐ)
        QString rootPath;
        QString destinationPath;
        QString relativePath;
        if (!validChangeType
                || qsnapper::restore::isSnapshotMetadataRestoreName(path)
                || !qsnapper::restore::buildRestoreDestination(
                    QStringLiteral("/"), path, &rootPath, &destinationPath, &relativePath)) {
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
 * @param manifestId owner束縛されたマニフェストID
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

    // 復元はlive filesystemを書き換える排他的な操作である
    // 複数計画がchunk境界で交互実行されると最終状態が非決定になり、
    // libsnapperのmount_user_requestがbool (カウンタではない) であることも相まってmount管理が衝突し得るため、同時に1計画のみ実行を許す
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

    // 認可対象は凍結済みの不変計画である
    // 認可待ちの間にcancel / TTL失効 / owner消失 / 他計画の実行開始が起こり得るため、実際のmountとexecutor起動は継続側で状態を再検証してから行う
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
 * @param manifestId 対象マニフェストID
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
 * 認可待ちの間にevent loopが回るため、凍結済み計画が生き残っている保証はない
 * mountやexecutor起動といった不可逆な操作の前に、以下を必ず再検証する:
 *   - 他の計画が実行を開始していないこと (復元はlive filesystemへの排他操作)
 *   - 計画がownerに束縛されたまま存在し、まだFrozenであること
 *     (cancel / TTL失効 / owner消失は全てここで弾かれる)
 *   - 凍結済みの全entryが、サーバが(source, counterpart)から再構築した
 *     権威ある比較と一致すること (クライアント申告のchangeTypeは信頼しない)
 *
 * @param manifestId owner束縛されたマニフェストID
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
        // Snapperは構築時にsnapshot一覧をcaptureし外部追加を観測しないため、
        // 権威ある比較の相手を解決する前に強制再生成する
        // cache clearが先である理由は、古いSnapperを参照するComparisonが
        // cacheへ残るのを防ぐためである
        m_comparisonCache.clear();
        snapper::Snapper *snapper = getSnapper(execution.configName,
                                               /*forceReload=*/true);
        if (!snapper) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Failed to initialize restore source"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        const snapper::Snapshots::const_iterator source =
            snapper->getSnapshots().find(execution.snapshotNumber);
        if (source == snapper->getSnapshots().end()) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore source snapshot is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // 復元元と同じ向きで比較相手を解決する
        // 0は「現在のシステム」のsentinelであり、正の値は番号で解決する
        // (Pre/Postの順序を推測・並べ替えるとCREATEDとDELETEDが反転する)
        snapper::Snapshots::const_iterator counterpart;
        if (status->counterpartSnapshotNumber == 0) {
            counterpart = snapper->getSnapshotCurrent();
        }
        else {
            counterpart = snapper->getSnapshots().find(
                status->counterpartSnapshotNumber);
        }
        if (counterpart == snapper->getSnapshots().end()
                || counterpart == source) {
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore comparison counterpart is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // 復元先の基準となるconfigのSUBVOLUMEを固定する
        // 計画に載る名前はconfig相対名であり、宛先は"<SUBVOLUME>/<名前>"として実行時に組み立てる
        execution.subvolume = QString::fromStdString(snapper->subvolumeDir());
        QString normalizedSubvolume;
        if (!qsnapper::restore::normalizeRestoreSubvolume(execution.subvolume,
                                                          &normalizedSubvolume)) {
            qWarning() << "Staged restore: Config subvolume is not usable as a restore root";
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore destination is unavailable"),
                &error);
            replyError(QDBusError::Failed,
                            QStringLiteral("Failed to prepare restore plan"));
            return false;
        }

        // root安全ネットの判定材料として、復元前のrootサブボリュームの状態を記録する
        // 元からread-onlyのroot (read-only snapshotからの起動など) を、復元後にrwへ変えないため
        execution.rootReadWriteState.targetsRootSubvolume =
            normalizedSubvolume == QStringLiteral("/");
        if (execution.rootReadWriteState.targetsRootSubvolume && m_rootReadOnlyProbe) {
            bool readOnly = true;
            execution.rootReadWriteState.preRestoreStateKnown =
                m_rootReadOnlyProbe(&readOnly);
            execution.rootReadWriteState.preRestoreReadOnly = readOnly;
        }

        source->mountFilesystemSnapshot(true);
        execution.mounted = true;
        const QString snapshotDir =
            QString::fromStdString(source->snapshotDir());
        m_restoreExecutions.insert(manifestId, execution);
        m_restoreExecutions[manifestId].snapshotDir = snapshotDir;

        // ソースsnapshotをdirfdでpinする
        // 以降のソース読み取りは本fd相対で行うため、chunk境界でevent loopが回る間にpath上のsnapshotが削除 / 同番号で再作成 / 差し替えられても、
        // 認可時に参照したsnapshotそのものから復元し続ける
        const int snapshotDirFd = ::open(snapshotDir.toUtf8().constData(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (snapshotDirFd < 0) {
            qWarning() << "Staged restore: Failed to pin snapshot dir:"
                       << strerror(errno);
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        m_restoreExecutions[manifestId].snapshotDirFd = snapshotDirFd;

        // pin済みfdの同一性判定に使うstatを確定させる
        struct stat pinnedStat = {};
        if (::fstat(snapshotDirFd, &pinnedStat) != 0) {
            qWarning() << "Staged restore: Failed to stat pinned snapshot dir:"
                       << strerror(errno);
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }

        // dirfdのpinで固定されるのはinodeだけで、内容は固定されない
        // 書き込み可能な復元元 (rollbackが作るwritable copyなど) では、認可の後に内容を差し替えられるため拒否する
        // libsnapperのメタデータと、pin済みfdに対するファイルシステムの状態の両方が読み取り専用であることを要求する
        std::string fstype;
        snapper->getConfigInfo().get_value("FSTYPE", fstype);
        if (!source->isReadOnly()
                || !qsnapper::restore::isPinnedRestoreSourceReadOnly(snapshotDirFd, fstype)) {
            qWarning() << "Staged restore: Restore source snapshot is not read-only";
            cleanupRestoreExecution(manifestId);
            m_restoreRegistry.markFailed(
                manifestId, owner,
                QStringLiteral("Restore source snapshot is not read-only"),
                &error);
            replyError(QDBusError::Failed,
                       QStringLiteral("Restore source snapshot is not read-only"));
            return false;
        }

        // executor起動前の最終検証
        // 権威ある比較の再構築と全entryの照合、pin済みdirfdの同一性確認が
        // 全て通るまで復元は1バイトもlive filesystemへ書き込まない
        try {
            if (!validateFrozenRestorePlan(manifestId, owner, snapper,
                                           source, counterpart, snapshotDir,
                                           pinnedStat)) {
                failRestorePlanPreflight(manifestId, owner, &error);
                return false;
            }
        }
        catch (const snapper::Exception &) {
            // 例外本文にはlibsnapperが扱ったfilesystem pathが含まれ得るため記録しない
            // (攻撃者由来の名前をログへ持ち込まない)
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (const std::exception &) {
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
        catch (...) {
            qWarning() << "Staged restore: Authoritative comparison reconstruction"
                       << "failed unexpectedly";
            failRestorePlanPreflight(manifestId, owner, &error);
            return false;
        }
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

    // 安全ネットは実行を開始した計画にだけ適用する
    // start()が失敗した場合はcleanupRestoreExecutionで実行contextごと破棄されるため、この印は残らない
    const auto startedExecution = m_restoreExecutions.find(manifestId);
    if (startedExecution != m_restoreExecutions.end()) {
        startedExecution->rootReadWriteState.executionStarted = true;
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
 * @brief 凍結済み計画をサーバ再構築の権威ある比較と照合して検証する
 * @param manifestId owner束縛されたマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @param snapper 検証済み設定のSnapperインスタンス
 * @param source 復元元snapshotのiterator
 * @param counterpart 比較相手snapshotのiterator
 * @param snapshotDir pin済みdirfdが指すsnapshot directoryのpath
 * @param pinnedStat pin済みdirfdに対するfstatの結果
 * @return 計画全体が権威ある比較と一致し同一性確認も通った場合true
 *
 * @note 本検証はbest-effortの整合性検査であり、tamper-proofなセキュリティ境界ではない
 *       snapper::Comparisonはread-only snapshot同士ではlibsnapper保存のfilelistキャッシュを
 *       優先して読み、そのキャッシュは無エスケープの行指向形式であるため、
 *       改行を含むファイル名により偽entryが混入し得る (詳細はrestorevalidation.hの注記)
 *       キャッシュを公開APIから無効化できないため本経路は塞げない
 *       実際の防護は実行段のisConfirmedAbsentAt()とsource種別判定が担い、
 *       こちらは認可時にpinしたdirfdから直接算出するためキャッシュの影響を受けない
 */
bool SnapshotOperations::validateFrozenRestorePlan(
    const QString &manifestId,
    const QString &owner,
    snapper::Snapper *snapper,
    const snapper::Snapshots::const_iterator &source,
    const snapper::Snapshots::const_iterator &counterpart,
    const QString &snapshotDir,
    const struct stat &pinnedStat)
{
    QHash<QString, QString> authoritative;

    {
        // ローカルの非キャッシュComparison (mount=false)
        // mount=falseでもinitialize()はstatus setを計算するため、
        // getName() / getPreToPostStatus()は使用可能である
        // blockを抜けるまでにComparisonを使い切り、cleanupやexecutor起動の
        // 前に確実に破棄する
        snapper::Comparison comparison(snapper, source, counterpart, false);

        const snapper::Files &files = comparison.getFiles();
        authoritative.reserve(static_cast<qsizetype>(files.size()));
        for (const snapper::File &file : files) {
            // 権威名は復号可能性と制御文字の両方を検証する
            // libsnapperのfilelistキャッシュは無エスケープの行指向形式であり、
            // 改行を含む名前はキャッシュ再読込時に偽entryとして混入し得る
            // 攻撃者が制御する名前はログにもエラー本文にも載せない
            QString authoritativeName;
            if (!qsnapper::restore::decodeAuthoritativeRestoreName(
                    file.getName(), &authoritativeName)) {
                qWarning() << "Staged restore: Authoritative comparison contains"
                           << "an unusable path name";
                return false;
            }

            if (!qsnapper::restore::registerAuthoritativeRestoreEntry(
                    &authoritative, authoritativeName,
                    file.getPreToPostStatus())) {
                // 同一パスに異なる期待型が潰れて衝突した場合は検証不能であり、
                // 計画全体をfail-closedさせる
                qWarning() << "Staged restore: Authoritative entries contain"
                           << "conflicting change types for one path";
                return false;
            }
        }

        qsnapper::restore::ManifestError validationError =
            qsnapper::restore::ManifestError::None;
        if (!qsnapper::restore::validateFrozenEntriesAgainstAuthoritative(
                m_restoreRegistry, manifestId, owner, authoritative,
                &validationError)) {
            qWarning() << "Staged restore: Frozen plan does not match the"
                       << "authoritative comparison";
            return false;
        }
    }

    // pin済みfdと、pathを改めて開いた結果が同一inodeであることを確認する
    // (st_devとst_inoの両方) 異なっていれば、検証対象と実行対象のスナップショットが入れ替わっているため実行を拒否する
    // 実行は以降もpin済みfdのみを通る
    struct stat currentStat = {};
    if (::stat(snapshotDir.toUtf8().constData(), &currentStat) != 0
            || currentStat.st_dev != pinnedStat.st_dev
            || currentStat.st_ino != pinnedStat.st_ino) {
        qWarning() << "Staged restore: Snapshot directory identity changed"
                   << "during validation";
        return false;
    }
    return true;
}

/**
 * @brief commit時preflight失敗を計画のfail-closed終端へ落とし込む
 * @param manifestId 検証に失敗したマニフェストID
 * @param owner 認可前にcaptureした呼び出し元unique name
 * @param error registryのmarkFailedへ渡すエラー格納先
 */
void SnapshotOperations::failRestorePlanPreflight(
    const QString &manifestId,
    const QString &owner,
    qsnapper::restore::ManifestError *error)
{
    cleanupRestoreExecution(manifestId);
    m_restoreRegistry.markFailed(
        manifestId, owner, QStringLiteral("Restore plan validation failed"),
        error);
    qWarning() << "Staged restore: Restore plan failed commit-time validation";
    replyError(QDBusError::Failed,
               QStringLiteral("Restore plan validation failed"));
}

/**
 * @brief owner確認済みRunning計画のidle loopを再開する
 * @param manifestId owner束縛されたマニフェストID
 * @return nudgeを受理した場合: true
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
 * @param manifestId owner束縛されたマニフェストID
 * @return ManifestStatusフィールド順のCSV、失敗時空文字列
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

    qsnapper::restore::ManifestError error = qsnapper::restore::ManifestError::None;
    auto status = m_restoreRegistry.status(manifestId, owner, &error);
    if (!status) {
        // 終端してregistryから削除された計画は、保持期間内であればownerにだけ詳細を返す
        status = m_finishedRestorePlans.status(manifestId, owner);
    }
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
 * @param manifestId owner束縛されたマニフェストID
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
 * @brief live pathをroot配下で再解決して一時的な兄弟pathへ退避する
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param path 退避対象の絶対path
 * @param movedPath 実際の退避先
 *                  対象不存在時は空文字列
 * @return 退避または対象不存在時true
 */
bool SnapshotOperations::movePathAsideBeneathRoot(const QString &rootPath,
                                                  const QString &path,
                                                  QString *movedPath)
{
    if (movedPath) {
        movedPath->clear();
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = siblingTemporaryPath(
            path, QStringLiteral("qsnapper-old"), attempt);
        if (qsnapper::security::safeRenamePathNoFollowBeneathRoot(rootPath, path, candidate)) {
            if (movedPath) {
                *movedPath = candidate;
            }
            return true;
        }

        if (errno == ENOENT) {
            return true;
        }

        // 退避先の名前が既に使われている場合 (RENAME_NOREPLACEによりEEXIST) は次の候補を試す
        // RENAME_NOREPLACE未対応 (EINVAL / ENOSYS) を含むその他のエラーでは、live側に触れずに失敗する
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
 * @param manifestId 実行contextを選択するマニフェストID
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
    // entry.pathはconfig相対名である
    // live側の宛先はcommit時に固定したSUBVOLUME配下に組み立て、以降の全てのlive操作はrootPathを基準に解決する
    // relativePathはrootPathからの相対であり、同時にpin済みスナップショット dirfdからの相対でもある
    QString rootPath;
    QString destinationPath;
    QString relativePath;
    if (!validChangeType
            || qsnapper::restore::isSnapshotMetadataRestoreName(entry.path)
            || !qsnapper::restore::buildRestoreDestination(
                context.subvolume, entry.path, &rootPath, &destinationPath, &relativePath)) {
        qWarning() << "Staged restore: Frozen entry failed execution-time validation";
        return false;
    }

    const QString snapshotFilePath = context.snapshotDir + QLatin1Char('/') + relativePath;
    if (!qsnapper::security::isPathWithinSnapshotRoot(
            snapshotFilePath, context.snapshotDir)) {
        qWarning() << "Staged restore: Source escaped snapshot root";
        return false;
    }

    if (entry.changeType == QStringLiteral("created")) {
        // "created"は「復元元snapshotに存在しない」という意味であり、だからこそlive側からの削除が正しい動作になる
        // 逆に復元元に存在するパスは、削除ではなく復元が正しい
        // サーバはクライアントが申告したentryを再計算しないため、シリアライズ経路への注入等で偽装されたentryがここまで到達し得る
        // 破壊の直前に前提そのものを確認する
        //
        // 検証は認可時にpinしたsnapshotDirFd相対で行い、本関数の冒頭でbuildRestoreDestinationがentry.pathから導出したrelativePathをそのまま使用する
        // 復元元snapshotはread-onlyかつfdでpin済みであり、検証対象と削除対象は同一のentry.pathに由来するため、検証後に別の値を取り直す余地はない
        if (!qsnapper::security::isConfirmedAbsentAt(context.snapshotDirFd, relativePath)) {
            qWarning() << "Staged restore: Refused to delete a path that is not confirmed"
                       << "absent from the restore source";
            return false;
        }

        const bool removed = qsnapper::security::safeRemoveAllBeneathRoot(rootPath, destinationPath);
        if (!removed) {
            qWarning() << "Staged restore: Failed to remove live path:"
                       << strerror(errno);
        }
        return removed;
    }

    // ソースの種別判定はpin済みsnapshot dirfd相対で行う
    // 実行中にsnapshotDirのpath上で何が起きても、認可時にpinしたinodeを観測する
    // (AT_SYMLINK_NOFOLLOWによりleafのsymlinkも展開しない)
    struct stat snapshotFileInfo;
    const bool hasSnapshotFileInfo = qsnapper::security::safeLstatAt(
        context.snapshotDirFd, relativePath, &snapshotFileInfo);

    // live側を破壊する前に復元元の可用性と種別を確定させる
    // 凍結・認可の時点でsnapshotDirを検証していても、認可から本entryの適用までにはchunk境界でevent loopが回るため、
    // その間に復元元snapshotが削除 / unmountされ得る
    // 先に退避・削除してから "Source is not restorable" で失敗すると、復元元が存在しないままlive側のデータだけが失われる
    const bool sourceIsLink = hasSnapshotFileInfo && S_ISLNK(snapshotFileInfo.st_mode);
    const bool sourceIsDirectory = hasSnapshotFileInfo && S_ISDIR(snapshotFileInfo.st_mode);
    const bool sourceIsRegular = hasSnapshotFileInfo && S_ISREG(snapshotFileInfo.st_mode);
    if (!sourceIsLink && !sourceIsDirectory && !sourceIsRegular) {
        qWarning() << "Staged restore: Source is not restorable";
        return false;
    }

    // 復元対象ではない欠けた親ディレクトリは、snapshot側の同じディレクトリに合わせて作成する
    // snapshot側に無い場合は0700・root所有のままにし、既存の親ディレクトリは変更しない
    const int slashIndex = destinationPath.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = slashIndex <= 0 ? QStringLiteral("/")
                                               : destinationPath.left(slashIndex);
    if (parentPath != rootPath
            && !qsnapper::security::safeCreateParentDirectoriesFromSourceBeneathRoot(
                rootPath, parentPath, context.snapshotDirFd, ::geteuid() == 0)) {
        qWarning() << "Staged restore: Failed to create live parent directory:"
                   << strerror(errno);
        return false;
    }

    QString detachedPath;
    if (context.removeOnTypechanged && entry.changeType == QStringLiteral("typechanged")) {
        if (!movePathAsideBeneathRoot(rootPath, destinationPath, &detachedPath)) {
            qWarning() << "Staged restore: Failed to move live path aside:"
                       << strerror(errno);
            return false;
        }
    }

    bool applied = false;
    if (sourceIsLink) {
        applied = copySymlinkBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath);
    }
    else if (sourceIsDirectory) {
        applied = applyDirectoryBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath);
    }
    else {
        applied = copyRegularFileBeneathRoot(context.snapshotDirFd, relativePath, rootPath, destinationPath, context.useReflink);
    }

    // 退避物の破棄は復元成功後にのみ行う
    // 失敗時は元の位置へ戻し、復元できなかったlive側のデータを消さない
    // 戻すことすらできない場合も退避物は削除せず、復旧できるようpathを記録する
    if (!detachedPath.isEmpty()) {
        if (applied) {
            if (!qsnapper::security::safeRemoveAllBeneathRoot(rootPath, detachedPath)) {
                qWarning() << "Staged restore: Failed to remove detached live path:"
                           << strerror(errno);
            }
        }
        else if (!qsnapper::security::safeRenamePathNoFollowBeneathRoot(rootPath, detachedPath, destinationPath)) {
            qCritical() << "Staged restore: Failed to reattach live path after a failed"
                        << "restore. Previous content is preserved at:" << detachedPath;
        }
    }

    return applied;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、ディレクトリを作成してmetadataを適用する
 *
 * 作成 (mkdirat) と、metadataを適用するためのopenを同じ親dirfd上で行い、名前を再解決しない
 * 既存のディレクトリはそのまま使い、所有者・mode・ACL (access / default)・xattrをsnapshot側に合わせる
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @return ディレクトリと必須metadataを適用できた場合true
 */
bool SnapshotOperations::applyDirectoryBeneathRoot(int sourceDirFd,
                                                   const QString &sourceRelativePath,
                                                   const QString &rootPath,
                                                   const QString &dst)
{
    const int srcFd = qsnapper::security::safeOpenDirectoryReadAt(sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to open source directory:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        ::close(srcFd);
        return false;
    }

    // 作成時は権限を絞り (0700)、所有者とmodeは後からfd経由で設定する
    int directoryFd = -1;
    if (::mkdirat(parentFd, leafName.constData(), 0700) == 0 || errno == EEXIST) {
        directoryFd = ::openat(parentFd, leafName.constData(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    const int openErrno = errno;
    ::close(parentFd);
    if (directoryFd < 0) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to create or open live directory:"
                   << strerror(openErrno);
        ::close(srcFd);
        return false;
    }

    const qsnapper::security::RestoredMetadataResult metadata =
        qsnapper::security::applyRestoredMetadata(srcFd, directoryFd, /*isDirectory=*/true);
    ::close(directoryFd);
    ::close(srcFd);

    if (metadata.optionalFailures > 0) {
        qWarning() << "applyDirectoryBeneathRoot:" << metadata.optionalFailures
                   << "extended attribute(s) could not be copied (non-fatal)";
    }
    if (metadata.mandatoryFailed) {
        qWarning() << "applyDirectoryBeneathRoot: Failed to preserve owner, mode or ACL:"
                   << strerror(metadata.mandatoryErrno);
        return ::geteuid() != 0;
    }

    return true;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、通常ファイルをコピーして差し替える
 *
 * live側を直接O_TRUNCで開くと、実行中のバイナリ (復元を実行しているqSnapper自身や稼働中のサービス) がETXTBSYで拒否され、既存inodeのhardlinkや緩いmodeも引き継いでしまう
 * そのため、同じ親dirfd上の一時ファイルへ書き出し、data → 所有者 → mode → ACL → xattr → capability → 時刻をfd経由で適用してから、renameat()で差し替える
 * 一時ファイルの作成からrenameまで名前を再解決しないため、書き込み可能な親ディレクトリを制御する攻撃者が途中で一時名や親を差し替えても、
 * 別objectをrenameしたり、攻撃者のobjectへmetadataを適用したりしない (差し替えはrenameの前後で検出して失敗にする)
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @param tryReflink FICLONEを先行試行するか
 * @return dataと必須metadataを適用して差し替えできた場合true
 */
bool SnapshotOperations::copyRegularFileBeneathRoot(int sourceDirFd,
                                                    const QString &sourceRelativePath,
                                                    const QString &rootPath,
                                                    const QString &dst,
                                                    bool tryReflink)
{
    const int srcFd = qsnapper::security::safeOpenRegularFileReadAt(
        sourceDirFd, sourceRelativePath);
    if (srcFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to open source:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        ::close(srcFd);
        return false;
    }

    // rootで動作する本番環境では、所有者・mode・ACL・capability・時刻を保てない復元は失敗扱いにする
    const bool mustPreserveMetadata = (::geteuid() == 0);
    qsnapper::security::RestoredMetadataResult metadata;
    const bool replaced = qsnapper::security::replaceRegularFileAt(
        parentFd, leafName, srcFd, tryReflink, mustPreserveMetadata, &metadata);
    const int replaceErrno = errno;
    ::close(parentFd);
    ::close(srcFd);

    if (metadata.mandatoryFailed) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to preserve owner, mode, ACL or capability:"
                   << strerror(metadata.mandatoryErrno);
    }
    if (metadata.optionalFailures > 0) {
        qWarning() << "copyRegularFileBeneathRoot:" << metadata.optionalFailures
                   << "extended attribute(s) could not be copied (non-fatal)";
    }
    if (!replaced) {
        qWarning() << "copyRegularFileBeneathRoot: Failed to replace destination:"
                   << strerror(replaceErrno);
        errno = replaceErrno;
        return false;
    }

    return true;
}

/**
 * @brief live宛先の親ディレクトリを1回だけ解決し、symlinkをコピーして差し替える
 *
 * 一時symlinkを同じ親dirfd上に作成し、所有者と時刻をfd経由で設定してから、renameat()で差し替える
 * 所有者と時刻の適用失敗は従来どおり非致命とする
 *
 * @param sourceDirFd pin済みのsnapshot dirfd
 * @param sourceRelativePath snapshotDirからの相対source path
 * @param rootPath 名前解決の基準とする絶対path (configのSUBVOLUME)
 * @param dst live filesystem上の絶対path
 * @return symlinkを作成して差し替えできた場合true
 */
bool SnapshotOperations::copySymlinkBeneathRoot(int sourceDirFd,
                                                const QString &sourceRelativePath,
                                                const QString &rootPath,
                                                const QString &dst)
{
    QByteArray linkTarget;
    struct stat srcStat;
    if (!qsnapper::security::safeReadLinkNoFollowAt(sourceDirFd, sourceRelativePath, &linkTarget)
            || !qsnapper::security::safeLstatAt(sourceDirFd, sourceRelativePath, &srcStat)) {
        qWarning() << "copySymlinkBeneathRoot: Failed to read source link:"
                   << strerror(errno);
        return false;
    }

    QByteArray leafName;
    const int parentFd = qsnapper::security::openDestinationParentBeneathRoot(rootPath, dst, &leafName);
    if (parentFd < 0) {
        qWarning() << "copySymlinkBeneathRoot: Failed to open destination parent:"
                   << strerror(errno);
        return false;
    }

    bool metadataApplied = false;
    const bool replaced = qsnapper::security::replaceSymlinkAt(
        parentFd, leafName, linkTarget, srcStat, &metadataApplied);
    const int replaceErrno = errno;
    ::close(parentFd);

    if (!replaced) {
        qWarning() << "copySymlinkBeneathRoot: Failed to replace destination:"
                   << strerror(replaceErrno);
        errno = replaceErrno;
        return false;
    }
    if (!metadataApplied) {
        qWarning() << "copySymlinkBeneathRoot: Failed to preserve link owner or times (non-fatal)";
    }

    return true;
}
