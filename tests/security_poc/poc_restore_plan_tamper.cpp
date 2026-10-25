/**
 * @file poc_restore_plan_tamper.cpp
 * @brief staged restoreのcommit時changeType検証 (復元元と比較相手の木の直接観測) のPoC実行体
 *
 * 本PoCは本番のルートファイルシステムに対しては絶対に実行しない
 * 全てのファイル操作は引数で与えたscratch root配下に閉じる (引数が"/"の場合は起動を拒否する)
 *
 * シナリオ:
 *   サーバはcommit時に、クライアントが申告したchangeTypeもlibsnapperの比較結果 (filelistキャッシュ) も信頼せず、
 *   復元元と比較相手の木を直接観測して、全エントリが実際の差分に裏付けられているかを照合する
 *   本PoCはpre / postの2つの「スナップショット」木を用意し、
 *   本番と同一の純粋関数群 (restorevalidation) と実security core
 *   (RestoreManifestRegistry / RestorePlanExecutor / qsnapper::security::*) を
 *   用いて以下を証明する:
 *     - STAGE 2a: 差分に現れるmodified エントリをcreatedへ改ざんした計画はcommit時に拒否され、live木は1バイトも変化しない
 *     - STAGE 2b: 差分に一切現れないパス (preとpostで同一内容) をmodifiedと偽って載せた計画もcommit時に拒否され、live木は不変である
 *                 (偽modifiedはスナップショット内容をliveへ上書きさせる攻撃になる)
 *     - STAGE 3:  正当なRevert to Pre計画 (pre --> postの向き) は検証を通り、実行後も差分外のパスは保持される (過剰拒否がないことの証明)
 *     - STAGE 4:  正当なRe-apply to Post計画 (post --> preの向き) も検証を通り、CREATED / DELETEDが向きに応じて正しく解釈される
 *
 * 検証対象の実装:
 *   - 木の観測による差分の裏付け: qsnapper::restore::restorevalidation と qsnapper::security::observePathNoFollowAt
 *   - 凍結済み計画の保持とbounded slice読み出し: RestoreManifestRegistry
 *   - 検証通過後の実行: RestorePlanExecutor + 実削除プリミティブ
 *     (qsnapper::security::safeRemoveAllBeneathRoot / isConfirmedAbsentAt)
 *
 * 実行:
 *   poc_restore_plan_tamper.sh [build-dir]   (tests/security_poc/README.mdの規約と同じ)
 *
 * Exitコード規約 (tests/security_poc/README.mdと同じ):
 *   0 = 期待どおり (阻止成功 + 両方向の正当計画が通過)
 *   1 = 想定外 (regression)
 *   2 = 環境エラー
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <optional>
#include <unistd.h>
#include "filesystemhelpers.h"
#include "inputvalidator.h"
#include "restoremanifest.h"
#include "restoreplanexecutor.h"
#include "restorevalidation.h"

namespace {

constexpr int kExitExpected = 0;      // 期待どおり (阻止成功と両方向の正当計画通過)
constexpr int kExitRegression = 1;    // 想定外のregression
constexpr int kExitEnvironment = 2;   // 環境エラー

QString g_scratchRoot;                // 全ファイル操作を閉じ込めるscratch root

/**
 * @brief 失敗を報告して即終了する
 * @param stage 失敗したSTAGE識別子
 * @param detail 詳細 (パスはscratch root配下の値のみ含む)
 */
void failNow(const QString &stage, const QString &detail)
{
    std::fprintf(stderr, "FAIL: %s: %s\n", qPrintable(stage), qPrintable(detail));
    std::exit(kExitRegression);
}

/**
 * @brief scratch root配下の絶対パスを解決する
 * @param relative rootからの相対パス ("/etc/x"のような絶対表現)
 * @return scratch rootを前置した実パス
 */
QString underRoot(const QString &relative)
{
    return g_scratchRoot + relative;
}

/**
 * @brief ファイルへ内容を書き込む (親ディレクトリも作成する)
 * @param relative scratch root配下の絶対表現パス
 * @param content 書き込む内容
 */
void writeFile(const QString &relative, const QString &content)
{
    const QString fullPath = underRoot(relative);
    QFileInfo info(fullPath);
    if (!QDir().mkpath(info.absolutePath())) {
        failNow(QStringLiteral("setup"), QStringLiteral("mkpath failed for %1")
                                            .arg(relative));
    }
    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(content.toUtf8()) < 0) {
        failNow(QStringLiteral("setup"), QStringLiteral("write failed for %1")
                                            .arg(relative));
    }
}

/**
 * @brief ファイルの内容を読み出す (不存在は空文字列ではなくstd::nullopt)
 * @param relative scratch root配下の絶対表現パス
 * @return 存在時は内容、不存在時はstd::nullopt
 */
std::optional<QString> readFile(const QString &relative)
{
    QFile file(underRoot(relative));
    if (!file.exists()) {
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        failNow(QStringLiteral("probe"), QStringLiteral("read failed for %1")
                                            .arg(relative));
    }
    return QString::fromUtf8(file.readAll());
}

/**
 * @brief live木の完全な状態スナップショットを取得する
 *
 * 相対パス -> (size, mtime, 内容) のmapであり、1バイトの変異も検出できる
 *
 * @param directory 走査対象ディレクトリ (実パス)
 * @param prefix 走査結果へ前置する絶対表現パス ("/etc"等)
 * @param snapshot 結果の格納先
 */
void snapshotTree(const QString &directory, const QString &prefix,
                  QHash<QString, QString> *snapshot)
{
    const QDir dir(directory);
    for (const QFileInfo &entry : dir.entryInfoList(
             QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString relative = prefix + QLatin1Char('/')
            + entry.fileName();
        if (entry.isDir()) {
            snapshotTree(entry.absoluteFilePath(), relative, snapshot);
        }
        else {
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                failNow(QStringLiteral("probe"), QStringLiteral("read failed for %1")
                                                    .arg(relative));
            }
            const QByteArray content = file.readAll();
            snapshot->insert(relative,
                             QStringLiteral("%1:%2:%3")
                                 .arg(entry.size())
                                 .arg(entry.lastModified().toMSecsSinceEpoch())
                                 .arg(QString::fromUtf8(content.toBase64())));
        }
    }
}

/**
 * @brief 計画をstagingから構築してfreezeする
 * @param registry 計画を保持するregistry
 * @param snapshotNumber 復元元スナップショット番号
 * @param counterpartSnapshotNumber 比較相手スナップショット番号
 * @param paths エントリのパス列
 * @param changeTypes エントリのchangeType列
 * @return 作成されたマニフェストID
 */
QString stageAndFreezePlan(qsnapper::restore::RestoreManifestRegistry &registry,
                           int snapshotNumber, int counterpartSnapshotNumber,
                           const QStringList &paths,
                           const QStringList &changeTypes)
{
    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.poc"), 1000, QStringLiteral("poc-root"), snapshotNumber,
        counterpartSnapshotNumber, qsnapper::restore::RestoreMode::DirectCopy,
        &error);
    if (id.isEmpty()) {
        failNow(QStringLiteral("setup"), QStringLiteral("createStaging failed"));
    }
    if (!registry.stageEntries(id, QStringLiteral(":1.poc"), paths,
                               changeTypes, &error)) {
        failNow(QStringLiteral("setup"), QStringLiteral("stageEntries failed"));
    }
    if (!registry.freeze(id, QStringLiteral(":1.poc"), &error)) {
        failNow(QStringLiteral("setup"), QStringLiteral("freeze failed"));
    }
    return id;
}

/**
 * @brief dirfdを根とする木を、本番と同じ規則で観測する関数を作る
 * @param rootFd 観測する木の根のdirfd (関数の利用中は開いたままにすること)
 * @return 相対パスを受け取り観測結果を返す関数
 */
qsnapper::restore::RestorePathObserver makeTreeObserver(int rootFd)
{
    return [rootFd](const QString &relativePath) {
        qsnapper::restore::RestorePathObservation observation;
        struct stat info = {};
        switch (qsnapper::security::observePathNoFollowAt(rootFd, relativePath, &info)) {
        case qsnapper::security::PathPresenceAt::Present:
            observation = qsnapper::restore::restorePathObservationFromStat(info);
            break;
        case qsnapper::security::PathPresenceAt::Absent:
            observation.presence = qsnapper::restore::RestorePathPresence::Absent;
            break;
        case qsnapper::security::PathPresenceAt::Unknown:
            observation.presence = qsnapper::restore::RestorePathPresence::Unknown;
            break;
        }
        return observation;
    };
}

/**
 * @brief commit時preflightを本番と同一の手順で実行する
 *
 * 復元元と比較相手の木を開き、全エントリが実際の差分に裏付けられているかだけを照合する
 * 認可と変異は行わない
 *
 * @param registry 凍結済み計画を保持するregistry
 * @param manifestId 対象マニフェストID
 * @param sourceTree 復元元スナップショット木 (scratch root配下の絶対表現パス)
 * @param counterpartTree 比較相手スナップショット木 (scratch root配下の絶対表現パス)
 * @return 計画全体が実際の差分に裏付けられた場合にtrueを返し、それ以外はfalseを返す
 */
bool runPreflight(qsnapper::restore::RestoreManifestRegistry &registry,
                  const QString &manifestId,
                  const QString &sourceTree,
                  const QString &counterpartTree)
{
    const int sourceFd = ::open(underRoot(sourceTree).toUtf8().constData(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int counterpartFd = ::open(underRoot(counterpartTree).toUtf8().constData(),
                                     O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (sourceFd < 0 || counterpartFd < 0) {
        failNow(QStringLiteral("preflight"), QStringLiteral("failed to open snapshot trees"));
    }

    qsnapper::restore::ManifestError error =
        qsnapper::restore::ManifestError::None;
    const bool accepted = qsnapper::restore::validateFrozenEntriesAgainstTrees(
        registry, manifestId, QStringLiteral(":1.poc"), QStringLiteral("/"),
        makeTreeObserver(sourceFd), makeTreeObserver(counterpartFd), &error);
    ::close(sourceFd);
    ::close(counterpartFd);
    return accepted;
}

} // namespace

/**
 * @brief エントリポイント
 * @param argc 引数の数
 * @param argv 引数配列 (argv[1] = scratch root)
 * @return 終了コード (tests/security_poc/README.mdの規約)
 */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-root>\n", argv[0]);
        return kExitEnvironment;
    }

    g_scratchRoot = QString::fromLocal8Bit(argv[1]);
    if (g_scratchRoot.isEmpty() || g_scratchRoot == QStringLiteral("/")
            || !QDir(g_scratchRoot).exists()) {
        std::fprintf(stderr, "ERROR: refusing to run: invalid scratch root: %s\n",
                     qPrintable(g_scratchRoot));
        return kExitEnvironment;
    }

    // --- STAGE 1: fixture構築 (pre / post / liveの3木) ---
    writeFile(QStringLiteral("/snapshots/pre/etc/modified.conf"), QStringLiteral("PRE\n"));
    writeFile(QStringLiteral("/snapshots/pre/etc/kept.conf"), QStringLiteral("SAME\n"));
    writeFile(QStringLiteral("/snapshots/pre/etc/deleted.conf"), QStringLiteral("EXISTED\n"));
    writeFile(QStringLiteral("/snapshots/post/etc/modified.conf"), QStringLiteral("POST\n"));
    // Btrfsのスナップショットと同じく、変更されていないファイルはinode番号と時刻を含めて同一に見えるよう、hard linkで共有する
    if (::link(underRoot(QStringLiteral("/snapshots/pre/etc/kept.conf")).toUtf8().constData(),
               underRoot(QStringLiteral("/snapshots/post/etc/kept.conf")).toUtf8().constData()) != 0) {
        failNow(QStringLiteral("setup"), QStringLiteral("link failed for /snapshots/post/etc/kept.conf"));
    }
    writeFile(QStringLiteral("/snapshots/post/etc/created.conf"), QStringLiteral("NEW\n"));

    // liveは「Post適用後 + ユーザの手違いで差分外ファイルを編集済み」の状態
    writeFile(QStringLiteral("/live/etc/modified.conf"), QStringLiteral("POST\n"));
    writeFile(QStringLiteral("/live/etc/kept.conf"), QStringLiteral("USER-EDITED\n"));
    writeFile(QStringLiteral("/live/etc/created.conf"), QStringLiteral("NEW\n"));

    std::printf("STAGE 1: fixture trees created under scratch root\n");

    // --- STAGE 2a: 差分のmodified エントリをcreatedへ改ざんした計画はcommit時に拒否される ---
    {
        qsnapper::restore::RestoreManifestRegistry registry;
        const QString id = stageAndFreezePlan(
            registry, 1, 2,
            {QStringLiteral("/etc/modified.conf")},
            {QStringLiteral("created")});

        QHash<QString, QString> liveBefore;
        snapshotTree(underRoot(QStringLiteral("/live")), QString(), &liveBefore);

        const bool accepted = runPreflight(registry, id, QStringLiteral("/snapshots/pre"),
                                           QStringLiteral("/snapshots/post"));
        std::printf("STAGE 2a: tampered (modified -> created) plan accepted = %s\n",
                    accepted ? "YES" : "NO");
        if (accepted) {
            failNow(QStringLiteral("STAGE 2a"),
                    QStringLiteral("tampered plan passed the commit-time validation"));
        }

        QHash<QString, QString> liveAfter;
        snapshotTree(underRoot(QStringLiteral("/live")), QString(), &liveAfter);
        std::printf("STAGE 2a: live tree unchanged = %s\n",
                    liveAfter == liveBefore ? "YES" : "NO");
        if (liveAfter != liveBefore) {
            failNow(QStringLiteral("STAGE 2a"),
                    QStringLiteral("live filesystem was mutated by a rejected plan"));
        }
    }

    // --- STAGE 2b: 差分に現れないパスをmodifiedと偽った計画も拒否される ---
    {
        qsnapper::restore::RestoreManifestRegistry registry;
        const QString id = stageAndFreezePlan(
            registry, 1, 2,
            {QStringLiteral("/etc/kept.conf")},
            {QStringLiteral("modified")});

        QHash<QString, QString> liveBefore;
        snapshotTree(underRoot(QStringLiteral("/live")), QString(), &liveBefore);

        const bool accepted = runPreflight(registry, id, QStringLiteral("/snapshots/pre"),
                                           QStringLiteral("/snapshots/post"));
        std::printf("STAGE 2b: forged out-of-diff modified plan accepted = %s\n",
                    accepted ? "YES" : "NO");
        if (accepted) {
            failNow(QStringLiteral("STAGE 2b"),
                    QStringLiteral("out-of-diff path passed the commit-time validation"));
        }

        QHash<QString, QString> liveAfter;
        snapshotTree(underRoot(QStringLiteral("/live")), QString(), &liveAfter);
        std::printf("STAGE 2b: live tree unchanged = %s\n",
                    liveAfter == liveBefore ? "YES" : "NO");
        if (liveAfter != liveBefore) {
            failNow(QStringLiteral("STAGE 2b"),
                    QStringLiteral("live filesystem was mutated by a rejected plan"));
        }
        // 攻撃対象の差分外ファイルが実際に無傷であることも内容で確認する
        const auto kept = readFile(QStringLiteral("/live/etc/kept.conf"));
        if (!kept.has_value() || kept.value() != QStringLiteral("USER-EDITED\n")) {
            failNow(QStringLiteral("STAGE 2b"),
                    QStringLiteral("out-of-diff file content changed"));
        }
    }

    // --- STAGE 3: 正当なRevert to Pre計画は検証を通り、差分外パスを保持する ---
    {
        qsnapper::restore::RestoreManifestRegistry registry;
        const QString id = stageAndFreezePlan(
            registry, 1, 2,
            {QStringLiteral("/etc/created.conf"),
             QStringLiteral("/etc/modified.conf"),
             QStringLiteral("/etc/deleted.conf")},
            {QStringLiteral("created"),
             QStringLiteral("modified"),
             QStringLiteral("deleted")});

        if (!runPreflight(registry, id, QStringLiteral("/snapshots/pre"),
                          QStringLiteral("/snapshots/post"))) {
            failNow(QStringLiteral("STAGE 3"),
                    QStringLiteral("legitimate revert-to-Pre plan was rejected"));
        }
        std::printf("STAGE 3: legitimate revert-to-Pre plan passed validation = YES\n");

        // 検証通過後のみexecutorを起動する (本番のcommit順序と同一)
        qsnapper::restore::RestorePlanExecutor executor(registry);
        const auto applyEntry = [](const QString &manifestId,
                                   const qsnapper::restore::RestoreEntry &entry) -> bool {
            Q_UNUSED(manifestId)
            if (entry.changeType == QStringLiteral("created")) {
                // 本番と同一の安全網: 復元元 (pre) スナップショットに存在するパスの削除は拒否される
                // 本番ではsplitDestinationBeneathRoot("/", entry.path)がrelativeを
                // 導出するため、PoCではscratch rootをrootとして同一の導出を行う
                const int sourceFd = ::open(underRoot(QStringLiteral("/snapshots/pre"))
                                                .toUtf8().constData(),
                                            O_RDONLY | O_DIRECTORY);
                if (sourceFd < 0) {
                    return false;
                }
                QString relative;
                if (!qsnapper::security::splitDestinationBeneathRoot(
                        g_scratchRoot, underRoot(entry.path), &relative)) {
                    ::close(sourceFd);
                    return false;
                }
                const bool confirmedAbsent =
                    qsnapper::security::isConfirmedAbsentAt(sourceFd, relative);
                ::close(sourceFd);
                if (!confirmedAbsent) {
                    return false;
                }
                // live木はscratch root配下の/liveに置かれているため、
                // 削除宛先はroot=scratch root、dest=/live + entry.pathとなる
                // (本番ではroot="/"、dest=entry.pathに相当する)
                return qsnapper::security::safeRemoveAllBeneathRoot(
                    g_scratchRoot, underRoot(QStringLiteral("/live") + entry.path));
            }
            // modified / deleted は復元元スナップショットからコピーする
            QFile source(underRoot(QStringLiteral("/snapshots/pre") + entry.path));
            if (QFileInfo(underRoot(QStringLiteral("/live") + entry.path)).isDir()) {
                return false;
            }
            QFile::remove(underRoot(QStringLiteral("/live") + entry.path));
            return source.copy(underRoot(QStringLiteral("/live") + entry.path));
        };
        // executorは適用結果を3値で受け取るため、bool値のPoC用処理を変換して渡す
        executor.setEntryApplier([applyEntry](const QString &manifestId,
                                              const qsnapper::restore::RestoreEntry &entry) {
            return applyEntry(manifestId, entry)
                    ? qsnapper::restore::RestoreEntryOutcome::Applied
                    : qsnapper::restore::RestoreEntryOutcome::Failed;
        });

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        if (!executor.start(id, QStringLiteral(":1.poc"), &error)) {
            failNow(QStringLiteral("STAGE 3"), QStringLiteral("executor start failed"));
        }

        const auto modified = readFile(QStringLiteral("/live/etc/modified.conf"));
        const auto deleted = readFile(QStringLiteral("/live/etc/deleted.conf"));
        const auto created = readFile(QStringLiteral("/live/etc/created.conf"));
        const auto kept = readFile(QStringLiteral("/live/etc/kept.conf"));
        const bool reverted = modified.has_value()
            && modified.value() == QStringLiteral("PRE\n")
            && deleted.has_value()
            && deleted.value() == QStringLiteral("EXISTED\n")
            && !created.has_value()
            && kept.has_value()
            && kept.value() == QStringLiteral("USER-EDITED\n");
        std::printf("STAGE 3: revert applied, out-of-diff file preserved = %s\n",
                    reverted ? "YES" : "NO");
        if (!reverted) {
            failNow(QStringLiteral("STAGE 3"),
                    QStringLiteral("revert-to-Pre produced an unexpected live state"));
        }
    }

    // --- STAGE 4: 正当なRe-apply to Post計画 (post --> preの向き) も検証を通る ---
    {
        // liveをPre適用後の状態へ戻す
        QFile::remove(underRoot(QStringLiteral("/live/etc/created.conf")));
        writeFile(QStringLiteral("/live/etc/modified.conf"), QStringLiteral("PRE\n"));
        writeFile(QStringLiteral("/live/etc/deleted.conf"), QStringLiteral("EXISTED\n"));
        writeFile(QStringLiteral("/live/etc/kept.conf"), QStringLiteral("USER-EDITED\n"));

        qsnapper::restore::RestoreManifestRegistry registry;
        // post --> preの向きでは、modified.confはmodified、deleted.confは「postに無くpreに在る」ためcreated、
        // created.confは「postに在りpreに無い」ためdeletedになる (向きでcreatedとdeletedが反転する)
        const QString id = stageAndFreezePlan(
            registry, 2, 1,
            {QStringLiteral("/etc/deleted.conf"),
             QStringLiteral("/etc/modified.conf"),
             QStringLiteral("/etc/created.conf")},
            {QStringLiteral("created"),
             QStringLiteral("modified"),
             QStringLiteral("deleted")});

        if (!runPreflight(registry, id, QStringLiteral("/snapshots/post"),
                          QStringLiteral("/snapshots/pre"))) {
            failNow(QStringLiteral("STAGE 4"),
                    QStringLiteral("legitimate re-apply-to-Post plan was rejected"));
        }
        std::printf("STAGE 4: legitimate re-apply-to-Post plan passed validation = YES\n");

        qsnapper::restore::RestorePlanExecutor executor(registry);
        const auto applyEntry = [](const QString &manifestId,
                                   const qsnapper::restore::RestoreEntry &entry) -> bool {
            Q_UNUSED(manifestId)
            if (entry.changeType == QStringLiteral("created")) {
                return qsnapper::security::safeRemoveAllBeneathRoot(
                    g_scratchRoot, underRoot(QStringLiteral("/live") + entry.path));
            }
            QFile source(underRoot(QStringLiteral("/snapshots/post") + entry.path));
            QFile::remove(underRoot(QStringLiteral("/live") + entry.path));
            return source.copy(underRoot(QStringLiteral("/live") + entry.path));
        };
        // executorは適用結果を3値で受け取るため、bool値のPoC用処理を変換して渡す
        executor.setEntryApplier([applyEntry](const QString &manifestId,
                                              const qsnapper::restore::RestoreEntry &entry) {
            return applyEntry(manifestId, entry)
                    ? qsnapper::restore::RestoreEntryOutcome::Applied
                    : qsnapper::restore::RestoreEntryOutcome::Failed;
        });

        qsnapper::restore::ManifestError error =
            qsnapper::restore::ManifestError::None;
        if (!executor.start(id, QStringLiteral(":1.poc"), &error)) {
            failNow(QStringLiteral("STAGE 4"), QStringLiteral("executor start failed"));
        }

        const auto modified = readFile(QStringLiteral("/live/etc/modified.conf"));
        const auto created = readFile(QStringLiteral("/live/etc/created.conf"));
        const auto deleted = readFile(QStringLiteral("/live/etc/deleted.conf"));
        const auto kept = readFile(QStringLiteral("/live/etc/kept.conf"));
        const bool reapplied = modified.has_value()
            && modified.value() == QStringLiteral("POST\n")
            && created.has_value()
            && created.value() == QStringLiteral("NEW\n")
            && !deleted.has_value()
            && kept.has_value()
            && kept.value() == QStringLiteral("USER-EDITED\n");
        std::printf("STAGE 4: re-apply applied, out-of-diff file preserved = %s\n",
                    reapplied ? "YES" : "NO");
        if (!reapplied) {
            failNow(QStringLiteral("STAGE 4"),
                    QStringLiteral("re-apply-to-Post produced an unexpected live state"));
        }
    }

    std::printf("RESULT: PASS (commit-time changeType validation blocks tampered"
                " plans and accepts legitimate plans in both directions)\n");
    return kExitExpected;
}
