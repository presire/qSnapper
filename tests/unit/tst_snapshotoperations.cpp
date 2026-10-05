#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <cstring>

#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filesystemhelpers.h"
#include "snapshotoperations.h"

/**
 * @brief SnapshotOperationsの復元計画まわりの特権境界を検証するテスト
 *
 * D-Busを介さずにスロットを直接呼ぶ (呼び出し元ownerは空文字列になる)
 * rootサブボリュームへの操作はテスト用の差し替え関数で観測し、実システムには触れない
 */
class TestSnapshotOperations : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void beginThenCancelDoesNotTouchRootSubvolume();
    void stagedPlanCancelDoesNotTouchRootSubvolume();

    // --- 差分生成の資源上限 (M5) ---
    void diffStillProducesUnifiedOutputForSmallChanges();
    void diffOmitsBinaryFiles();
    void diffOmitsTooLargeFiles();
    void diffOmitsFilesWithTooManyLines();
    void diffDoesNotOpenFifo();
    void diffOmitsTooManyChanges();
    void diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput();

    // --- 書き込み可能な復元元の拒否 (M8) ---
    void writableDirectoryIsNotReadOnlyRestoreSource();
    void btrfsSubvolumeReadOnlyFlagIsRequired();
    void readOnlyMountIsAcceptedForNonBtrfs();

private:
    /**
     * @brief rootサブボリュームへの操作を観測する差し替え関数を設定する
     * @param operations 対象のSnapshotOperations
     */
    void installRootSubvolumeSpies(SnapshotOperations &operations);

    int m_readOnlyProbeCalls = 0;
    int m_readWriteRestoreCalls = 0;
};

namespace {

    /**
     * @brief テスト用ファイルを作成する
     * @param path 作成先
     * @param content 内容
     * @return 作成できた場合true
     */
    bool writeFile(const QString &path, const QByteArray &content)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write(content) == content.size();
    }

    /**
     * @brief 指定した接頭辞と連番で行を作る
     * @param prefix 行の接頭辞
     * @param count 行数
     * @return 改行区切りの内容
     */
    QByteArray numberedLines(const QByteArray &prefix, int count)
    {
        QByteArray content;
        for (int i = 0; i < count; ++i) {
            content += prefix + QByteArray::number(i) + '\n';
        }
        return content;
    }

}

void TestSnapshotOperations::init()
{
    m_readOnlyProbeCalls = 0;
    m_readWriteRestoreCalls = 0;
}

void TestSnapshotOperations::installRootSubvolumeSpies(SnapshotOperations &operations)
{
    operations.setRootSubvolumeAccessForTesting(
        [this](bool *readOnly) {
            ++m_readOnlyProbeCalls;
            *readOnly = true;
            return true;
        },
        [this]() {
            ++m_readWriteRestoreCalls;
            return true;
        });
}

void TestSnapshotOperations::beginThenCancelDoesNotTouchRootSubvolume()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    // 認可を要求しないBegin -> Cancelだけで、root権限のrw化へ到達してはならない
    const QString manifestId = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!manifestId.isEmpty());

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    QVERIFY(operations.CancelRestorePlan(manifestId));

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), manifestId);
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);
}

void TestSnapshotOperations::stagedPlanCancelDoesNotTouchRootSubvolume()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    const QString manifestId = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("yast"));
    QVERIFY(!manifestId.isEmpty());
    QVERIFY(operations.StageRestoreEntries(manifestId,
                                           {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    QVERIFY(operations.CancelRestorePlan(manifestId));

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);
}

void TestSnapshotOperations::diffStillProducesUnifiedOutputForSmallChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\r\nb\nc\n"));
    QVERIFY(writeFile(newPath, "a\nB\nc\n"));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QCOMPARE(result.text,
             QStringLiteral("--- %1\n+++ %2\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n").arg(oldPath, newPath));

    // 差分が無い場合は空で、省略理由も付かない
    const qsnapper::diff::UnifiedDiffResult same = qsnapper::diff::generateUnifiedDiff(newPath, newPath);
    QVERIFY(same.text.isEmpty());
    QVERIFY(same.omittedReason.isEmpty());
}

void TestSnapshotOperations::diffOmitsBinaryFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString binaryPath = tempDir.filePath(QStringLiteral("binary"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(writeFile(binaryPath, QByteArray("ELF\0\1\2", 6)));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(textPath, binaryPath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("binary"));
}

void TestSnapshotOperations::diffOmitsTooLargeFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString smallPath = tempDir.filePath(QStringLiteral("small"));
    const QString largePath = tempDir.filePath(QStringLiteral("large"));
    QVERIFY(writeFile(smallPath, "line\n"));

    const qsnapper::diff::UnifiedDiffLimits defaults;
    QVERIFY(writeFile(largePath, QByteArray(static_cast<qsizetype>(defaults.maxFileBytes + 1), 'x')));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(smallPath, largePath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

void TestSnapshotOperations::diffOmitsFilesWithTooManyLines()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\n"));
    QVERIFY(writeFile(newPath, QByteArray(64, '\n')));

    qsnapper::diff::UnifiedDiffLimits limits;
    limits.maxLinesPerFile = 32;
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, limits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

void TestSnapshotOperations::diffDoesNotOpenFifo()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString fifoPath = tempDir.filePath(QStringLiteral("fifo"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(::mkfifo(fifoPath.toUtf8().constData(), 0600) == 0);

    // 書き込み側のいないFIFOを開くと永久にブロックするため、開かずに省略理由を返す必要がある
    QElapsedTimer timer;
    timer.start();
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(fifoPath, textPath);
    QVERIFY(timer.elapsed() < 5000);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("special_file"));

    // 読み込み用helperもFIFOを開かない
    errno = 0;
    QCOMPARE(qsnapper::security::safeOpenRegularFileRead(fifoPath), -1);
    QCOMPARE(errno, EINVAL);
}

void TestSnapshotOperations::diffOmitsTooManyChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, numberedLines("old ", 2000)));
    QVERIFY(writeFile(newPath, numberedLines("new ", 2000)));

    // 作業領域の上限
    qsnapper::diff::UnifiedDiffLimits traceLimits;
    traceLimits.maxTraceBytes = 256 * 1024;
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, traceLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 探索ステップ数の上限
    qsnapper::diff::UnifiedDiffLimits stepLimits;
    stepLimits.maxSteps = 10000;
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, stepLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 既定の上限内であれば差分を生成する
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QVERIFY(result.text.contains(QStringLiteral("@@ -1,2000 +1,2000 @@")));
}

void TestSnapshotOperations::diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));

    // 全行が異なる (編集距離が最大) 入力と、同一行の繰り返しに異なる行を混ぜた (全対角線のsnakeが長い) 入力
    QVERIFY(writeFile(oldPath, numberedLines("a", 200000)));
    QVERIFY(writeFile(newPath, numberedLines("b", 200000)));

    QElapsedTimer timer;
    timer.start();
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    QByteArray repeated;
    QByteArray interleaved;
    for (int i = 0; i < 200000; ++i) {
        repeated += "x\n";
        interleaved += (i % 2 == 0) ? "x\n" : "y\n";
    }
    QVERIFY(writeFile(oldPath, repeated));
    QVERIFY(writeFile(newPath, interleaved));
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    qInfo("pathological diff inputs took %lld ms", static_cast<long long>(timer.elapsed()));
    QVERIFY(timer.elapsed() < 10000);
}

// ============================================================================
// 書き込み可能な復元元の拒否 (M8)
// ============================================================================

namespace {

    /**
     * @brief ディレクトリをO_DIRECTORYで開く
     * @param path 対象
     * @return fd。失敗時は-1
     */
    int openDirectory(const QString &path)
    {
        return ::open(path.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

}

void TestSnapshotOperations::writableDirectoryIsNotReadOnlyRestoreSource()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int dirFd = openDirectory(tempDir.path());
    QVERIFY(dirFd >= 0);
    // btrfs以外の場所でFSTYPE=btrfsと申告されても、subvolumeとして確認できなければ拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs"));
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs"));
    ::close(dirFd);
}

void TestSnapshotOperations::btrfsSubvolumeReadOnlyFlagIsRequired()
{
    const QString base = QDir::homePath();
    struct statfs fsInfo;
    if (::statfs(base.toUtf8().constData(), &fsInfo) < 0
            || fsInfo.f_type != BTRFS_SUPER_MAGIC) {
        QSKIP("HOME is not on btrfs");
    }

    QTemporaryDir tempDir(base + QStringLiteral("/.qsnapper-test-XXXXXX"));
    QVERIFY(tempDir.isValid());

    const int parentFd = openDirectory(tempDir.path());
    QVERIFY(parentFd >= 0);
    struct btrfs_ioctl_vol_args args;
    std::memset(&args, 0, sizeof(args));
    std::strncpy(args.name, "snapshot", sizeof(args.name) - 1);
    const int created = ::ioctl(parentFd, BTRFS_IOC_SUBVOL_CREATE, &args);
    ::close(parentFd);
    if (created < 0) {
        QSKIP("Cannot create a btrfs subvolume as this user");
    }

    const QString subvolume = tempDir.path() + QStringLiteral("/snapshot");
    const int subvolumeFd = openDirectory(subvolume);
    QVERIFY(subvolumeFd >= 0);

    // 書き込み可能なsubvolume (rollbackのwritable copy相当) は拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));

    quint64 flags = BTRFS_SUBVOL_RDONLY;
    const bool flagged = ::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0;
    if (flagged) {
        QVERIFY(qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));
        flags = 0;
        QVERIFY(::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0);
    }
    ::close(subvolumeFd);

    // 後片付け: 空のsubvolumeは所有者がrmdirできる (kernel 4.18以降)
    if (::rmdir(subvolume.toUtf8().constData()) < 0) {
        qWarning("Could not remove test subvolume: %s", std::strerror(errno));
    }
    if (!flagged) {
        QSKIP("Cannot set the btrfs read-only flag as this user");
    }
}

void TestSnapshotOperations::readOnlyMountIsAcceptedForNonBtrfs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray mountPoint = tempDir.path().toUtf8();

    // 非特権のuser + mount namespaceで、読み取り専用のtmpfsをmountして確認する
    constexpr int kNamespaceUnavailable = 77;
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        const auto writeText = [](const char *path, const QByteArray &text) {
            const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            const bool written = fd >= 0 && ::write(fd, text.constData(), text.size()) == text.size();
            if (fd >= 0) {
                ::close(fd);
            }
            return written;
        };
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0
                || !writeText("/proc/self/setgroups", "deny")
                || !writeText("/proc/self/uid_map", "0 " + QByteArray::number(uid) + " 1")
                || !writeText("/proc/self/gid_map", "0 " + QByteArray::number(gid) + " 1")
                || ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0
                || ::mount("tmpfs", mountPoint.constData(), "tmpfs", MS_RDONLY, "size=1m") < 0) {
            ::_exit(kNamespaceUnavailable);
        }
        const int dirFd = ::open(mountPoint.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            ::_exit(1);
        }
        const bool accepted = qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs");
        // 読み取り専用mountでも、btrfsのsubvolumeでなければbtrfsとしては受け入れない
        const bool rejectedAsBtrfs = !qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs");
        ::_exit(accepted && rejectedAsBtrfs ? 0 : 2);
    }

    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);
    QVERIFY(WIFEXITED(status));
    if (WEXITSTATUS(status) == kNamespaceUnavailable) {
        QSKIP("Unprivileged user and mount namespaces are unavailable");
    }
    QCOMPARE(WEXITSTATUS(status), 0);
}

QTEST_GUILESS_MAIN(TestSnapshotOperations)
#include "tst_snapshotoperations.moc"
