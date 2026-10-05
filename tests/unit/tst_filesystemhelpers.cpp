#include <QtTest/QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "filesystemhelpers.h"

using namespace qsnapper::security;

class TestFilesystemHelpers : public QObject
{
    Q_OBJECT

private slots:
    void safeMkpathCreatesNestedDirectories();
    void safeMkpathRejectsSymlinkComponents();
    void safeMkpathRejectsTargetSymlink();
    void safeOpenRegularFileReadRejectsSymlink();
    void safeOpenDirectoryRejectsSymlinkPath();
    void safeReadLinkNoFollowHandlesLargeTarget();
    void safeLstatReportsSymlinkItself();
    void safeRemoveAllRemovesSingleFile();
    void safeRemoveAllRemovesTreeWithoutFollowingSymlink();
    void safeRemoveAllRejectsSymlinkIntermediateComponent();
    void safeRemoveAllStopsAtMountBoundary();
    void safeRemoveAllRefusesMountPointTarget();
    void safeRemoveAllStopsAtSameFilesystemBindMount();
    void safeRemoveAllBeneathRootStopsAtMountBoundary();
    void safeRemoveAllStopsAtNestedBtrfsSubvolume();

    // --- 復元時のmetadata保持とrename前後の差し替え検出 ---
    void replaceRegularFileAtReplacesContentWithNewInode();
    void replaceRegularFileAtPreservesSetuidAndSetgid();
    void replaceRegularFileAtCopiesAccessAcl();
    void replaceRegularFileAtDropsInheritedDefaultAcl();
    void applyRestoredMetadataSyncsDirectoryAcls();
    void replaceRegularFileAtDetectsTemporaryNameSwap();
    void replaceRegularFileAtKeepsResolvedParentAfterParentSwap();
    void replaceSymlinkAtAppliesMetadataAndDetectsSwap();

    // --- beneath-root 宛先解決ハードニング (Todo 2) ---
    void splitDestinationBeneathRootAcceptsNestedDestination();
    void splitDestinationBeneathRootRejectsDotDotTraversal();
    void splitDestinationBeneathRootRejectsOutsideRoot();
    void splitDestinationBeneathRootRejectsMalformedPaths();
    void safeOpenDirectoryBeneathRootWalksNestedPath();
    void safeOpenDirectoryBeneathRootCreatesMissing();
    void safeOpenDirectoryBeneathRootRejectsSymlinkComponent();
    void beneathRootWriteRejectsParentSymlinkSwap();
    void beneathRootMkdirRejectsParentSymlinkSwap();
    void beneathRootRenameAsideRejectsParentSymlinkSwap();
    void beneathRootRemoveRejectsParentSymlinkSwap();
    void beneathRootSymlinkCreateRejectsParentSymlinkSwap();
    void beneathRootWriteRejectsGrandparentSymlinkSwap();
    void beneathRootMkdirRejectsGrandparentSymlinkSwap();
    void beneathRootWriteRejectsLeafSymlinkToExternalFile();
    void beneathRootWriteRejectsDanglingSymlinkComponent();
    void beneathRootWriteRejectsRegularFileAsDirectoryComponent();
    void beneathRootHappyPathWritesNestedFileWithMode();
    void beneathRootRenameAsideMovesWithinRoot();
    void beneathRootRenameAsideDoesNotReplaceExisting_data();
    void beneathRootRenameAsideDoesNotReplaceExisting();
    void beneathRootMissingParentsFollowSnapshotMetadata();
    void beneathRootMissingParentsDoNotModifyExistingParents();
    void beneathRootMissingParentsRejectLiveSymlinkComponent();
    void snapshotListFingerprintDetectsListChanges();
    void snapshotListFingerprintDetectsLeadingZeroEntries();
    void parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode();
    void beneathRootMissingParentsFollowTrustedParentPolicy();
    void beneathRootSymlinkHelpersApplyWithinRoot();
    void beneathRootStaleResolutionStillRejected();
    void beneathRootRepeatedSwapsDoNotLeakDescriptors();
    void beneathRootHandlesMalformedInputsSafely();
    void beneathRootRejectsSymlinkInRootPathAncestry();

    // --- dirfd相対ソース解決 (staged restore のsnapshot pin) ---
    void safeLstatAtResolvesRelativeToDirFd();
    void safeLstatAtRejectsAbsoluteAndTraversalPaths();
    void safeOpenRegularFileReadAtResolvesRelativeToDirFd();
    void safeOpenRegularFileReadAtRejectsSymlinkLeafAndNonRegular();
    void safeReadLinkNoFollowAtReadsRelativeToDirFd();
    void safeReadLinkNoFollowAtRejectsInvalidPaths();

    // --- created entry削除前の復元元不在確認 ---
    void isConfirmedAbsentAtRejectsPathsPresentInSource();
    void isConfirmedAbsentAtConfirmsOnlyDefiniteAbsence();
    void isConfirmedAbsentAtRefusesIntermediateSymlinks();

    // --- dirfd相対ソース解決の中間成分in-rootハードニング (RESOLVE_IN_ROOT) ---
    void safeOpenRegularFileReadAtKeepsAbsoluteIntermediateSymlinkInsideRoot();
    void safeOpenRegularFileReadAtClampsDotDotInSymlinkTargetAtRoot();
    void safeReadHelpersKeepLegitimateRelativeIntermediateSymlinkWorking();
    void safeOpenRegularFileReadAtResolvesSnapshotAbsoluteSymlinkInRoot();
    void safeHelpersKeepLeafSymlinkUnresolvedThroughIntermediateSymlink();
    void restoreReadHelpersHandleSnapshotWithIntermediateSymlinks();

    // --- openat2非対応カーネル向けフォールバックのin-root解決 ---
    void inRootFallbackResolvesIdenticallyToOpenat2();
    void inRootFallbackRejectsSymlinkLoop();

private:
    static bool isAttackRejectionErrno(int err);
    static bool isSafeHandlingErrno(int err);
    static QStringList fingerprintTree(const QString &dirPath);
    void buildAttackFixture(QTemporaryDir *rootDir, QTemporaryDir *outsideDir,
                            QString *victimPath, QByteArray *sentinel) const;
    static void swapComponentToSymlink(const QTemporaryDir &rootDir,
                                       const QTemporaryDir &outsideDir,
                                       const QStringList &components);
    void verifyOutsideUntouched(const QTemporaryDir &outsideDir, const QByteArray &sentinel,
                                const QStringList &fingerprintBefore) const;
};

void TestFilesystemHelpers::safeMkpathCreatesNestedDirectories()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString nestedPath = tempDir.path() + QStringLiteral("/a/b/c");
    QVERIFY(safeMkpath(nestedPath));
    QVERIFY(QDir(nestedPath).exists());
}

void TestFilesystemHelpers::safeMkpathRejectsSymlinkComponents()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realDir = tempDir.path() + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realDir));

    const QString symlinkPath = tempDir.path() + QStringLiteral("/link");
    const QByteArray target = realDir.toUtf8();
    const QByteArray link = symlinkPath.toUtf8();
    QVERIFY(::symlink(target.constData(), link.constData()) == 0);

    QVERIFY(!safeMkpath(symlinkPath + QStringLiteral("/child")));
    QVERIFY(!QDir(realDir + QStringLiteral("/child")).exists());
}

void TestFilesystemHelpers::safeMkpathRejectsTargetSymlink()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realDir = tempDir.path() + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realDir));

    const QString symlinkPath = tempDir.path() + QStringLiteral("/link");
    QVERIFY(::symlink(realDir.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    QVERIFY(!safeMkpath(symlinkPath));
}

void TestFilesystemHelpers::safeOpenRegularFileReadRejectsSymlink()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realFile = tempDir.path() + QStringLiteral("/real.txt");
    QFile file(realFile);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("safe");
    file.close();

    const QString symlinkPath = tempDir.path() + QStringLiteral("/link.txt");
    QVERIFY(::symlink(realFile.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    const int realFd = safeOpenRegularFileRead(realFile);
    QVERIFY(realFd >= 0);
    ::close(realFd);

    const int symlinkFd = safeOpenRegularFileRead(symlinkPath);
    QVERIFY(symlinkFd < 0);
}

void TestFilesystemHelpers::safeOpenDirectoryRejectsSymlinkPath()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realDir = tempDir.path() + QStringLiteral("/real-dir");
    QVERIFY(QDir().mkpath(realDir));

    const QString symlinkPath = tempDir.path() + QStringLiteral("/dir-link");
    QVERIFY(::symlink(realDir.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    const int fd = safeOpenDirectory(symlinkPath);
    QVERIFY(fd < 0);
}

void TestFilesystemHelpers::safeReadLinkNoFollowHandlesLargeTarget()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray largeTarget(2048, 'a');
    const QString linkPath = tempDir.path() + QStringLiteral("/large-link");
    QVERIFY(::symlink(largeTarget.constData(), linkPath.toUtf8().constData()) == 0);

    const int dirFd = ::open(tempDir.path().toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);
    QByteArray observedTarget;
    QVERIFY(safeReadLinkNoFollowAt(dirFd, QStringLiteral("large-link"), &observedTarget));
    ::close(dirFd);
    QCOMPARE(observedTarget, largeTarget);
}

void TestFilesystemHelpers::safeLstatReportsSymlinkItself()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realFile = tempDir.path() + QStringLiteral("/real.txt");
    {
        QFile file(realFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("safe");
    }

    const QString symlinkPath = tempDir.path() + QStringLiteral("/link.txt");
    QVERIFY(::symlink(realFile.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    struct stat st;
    QVERIFY(safeLstat(symlinkPath, &st));
    QVERIFY(S_ISLNK(st.st_mode));
}

void TestFilesystemHelpers::safeRemoveAllRemovesTreeWithoutFollowingSymlink()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outsideFile = tempDir.path() + QStringLiteral("/outside.txt");
    {
        QFile file(outsideFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("outside");
    }

    const QString treePath = tempDir.path() + QStringLiteral("/tree");
    QVERIFY(QDir().mkpath(treePath + QStringLiteral("/nested")));
    {
        QFile file(treePath + QStringLiteral("/nested/file.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("inside");
    }

    const QString symlinkPath = treePath + QStringLiteral("/link.txt");
    QVERIFY(::symlink(outsideFile.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    QVERIFY(safeRemoveAllBeneathRoot(QStringLiteral("/"), treePath));
    QVERIFY(!QDir(treePath).exists());

    QFile verify(outsideFile);
    QVERIFY(verify.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(verify.readAll(), QByteArray("outside"));
}

void TestFilesystemHelpers::safeRemoveAllRemovesSingleFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/single.txt");
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("only");
    }

    QVERIFY(safeRemoveAllBeneathRoot(QStringLiteral("/"), filePath));
    QVERIFY(!QFile::exists(filePath));
}

void TestFilesystemHelpers::safeRemoveAllRejectsSymlinkIntermediateComponent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realDir = tempDir.path() + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realDir));

    const QString realFile = realDir + QStringLiteral("/target.txt");
    {
        QFile file(realFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("target");
    }

    const QString symlinkPath = tempDir.path() + QStringLiteral("/link");
    QVERIFY(::symlink(realDir.toUtf8().constData(), symlinkPath.toUtf8().constData()) == 0);

    const QString victimPath = symlinkPath + QStringLiteral("/target.txt");
    QVERIFY(!safeRemoveAllBeneathRoot(QStringLiteral("/"), victimPath));
    QVERIFY(QFile::exists(realFile));
}

// ============================================================================
// 再帰削除のmount境界検出
// ============================================================================

namespace {

    constexpr int kNamespaceUnavailable = 77;

    /**
     * @brief 文字列をファイルへ書き込む (子プロセス内でも使えるよう、Qtを使わない)
     * @param path 書き込み先
     * @param text 書き込む内容
     * @return 書き込めた場合true
     */
    bool writeTextFile(const char *path, const char *text)
    {
        const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        const ssize_t length = static_cast<ssize_t>(std::strlen(text));
        const bool ok = ::write(fd, text, static_cast<size_t>(length)) == length;
        ::close(fd);
        return ok;
    }

    /**
     * @brief 非特権のuser + mount namespaceを持つ子プロセスでbodyを実行する
     *
     * root権限なしでmountを作るため、子プロセスでunshare(CLONE_NEWUSER | CLONE_NEWNS)し、自uid/gidをnamespace内のrootへ対応付ける
     * 子プロセスの終了とともにmountも消えるため、テスト後の後片付けが不要になる
     *
     * @param body 子プロセスで実行する検証処理 (0: 成功、それ以外: 失敗箇所を示すコード)
     * @return bodyの戻り値、namespaceを作れない環境ではkNamespaceUnavailable、異常終了時は-1
     */
    int runInPrivateMountNamespace(const std::function<int()> &body)
    {
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        const pid_t pid = ::fork();
        if (pid < 0) {
            return -1;
        }
        if (pid == 0) {
            if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0) {
                ::_exit(kNamespaceUnavailable);
            }
            const QByteArray uidMap = "0 " + QByteArray::number(uid) + " 1";
            const QByteArray gidMap = "0 " + QByteArray::number(gid) + " 1";
            if (!writeTextFile("/proc/self/setgroups", "deny")
                    || !writeTextFile("/proc/self/uid_map", uidMap.constData())
                    || !writeTextFile("/proc/self/gid_map", gidMap.constData())
                    || ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
                ::_exit(kNamespaceUnavailable);
            }
            ::_exit(body());
        }

        int status = 0;
        if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
            return -1;
        }
        return WEXITSTATUS(status);
    }

    /**
     * @brief 子プロセスで空ファイルを作成する
     * @param path 作成先
     * @return 作成できた場合true
     */
    bool createEmptyFile(const QString &path)
    {
        const int fd = ::open(path.toUtf8().constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }
        ::close(fd);
        return true;
    }

    /**
     * @brief symlink非追従でパスの存在を確認する
     * @param path 確認対象
     * @return 存在する場合true
     */
    bool existsNoFollow(const QString &path)
    {
        struct stat st;
        return ::lstat(path.toUtf8().constData(), &st) == 0;
    }

}

void TestFilesystemHelpers::safeRemoveAllStopsAtMountBoundary()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString tree = tempDir.path() + QStringLiteral("/tree");
    const QString mountPoint = tree + QStringLiteral("/usb");
    QVERIFY(QDir().mkpath(tree + QStringLiteral("/plain")));
    QVERIFY(QDir().mkpath(mountPoint));
    QVERIFY(createEmptyFile(tree + QStringLiteral("/plain/file")));

    const int result = runInPrivateMountNamespace([&]() {
        if (::mount("tmpfs", mountPoint.toUtf8().constData(), "tmpfs", 0, "size=1m") < 0) {
            return kNamespaceUnavailable;
        }
        if (::mkdir((mountPoint + QStringLiteral("/data")).toUtf8().constData(), 0755) < 0
                || !createEmptyFile(mountPoint + QStringLiteral("/data/inside"))
                || !createEmptyFile(mountPoint + QStringLiteral("/top"))) {
            return 1;
        }

        errno = 0;
        if (safeRemoveAllBeneathRoot(QStringLiteral("/"), tree)) {
            return 2;
        }
        if (errno != EXDEV) {
            return 3;
        }
        // 境界の内側は1件も削除されない
        if (!existsNoFollow(mountPoint + QStringLiteral("/data/inside"))
                || !existsNoFollow(mountPoint + QStringLiteral("/top"))) {
            return 4;
        }
        // 境界の外側にある親ディレクトリは残る (中身を失ったmount pointの上位を消さない)
        if (!existsNoFollow(tree)) {
            return 5;
        }
        return 0;
    });

    if (result == kNamespaceUnavailable) {
        QSKIP("Unprivileged user/mount namespaces are not available");
    }
    QCOMPARE(result, 0);
}

void TestFilesystemHelpers::safeRemoveAllRefusesMountPointTarget()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString mountPoint = tempDir.path() + QStringLiteral("/mnt");
    QVERIFY(QDir().mkpath(mountPoint));

    const int result = runInPrivateMountNamespace([&]() {
        if (::mount("tmpfs", mountPoint.toUtf8().constData(), "tmpfs", 0, "size=1m") < 0) {
            return kNamespaceUnavailable;
        }
        if (!createEmptyFile(mountPoint + QStringLiteral("/inside"))) {
            return 1;
        }

        // 削除対象そのものがmount pointである場合も降りない
        errno = 0;
        if (safeRemoveAllBeneathRoot(QStringLiteral("/"), mountPoint)) {
            return 2;
        }
        if (errno != EXDEV) {
            return 3;
        }
        return existsNoFollow(mountPoint + QStringLiteral("/inside")) ? 0 : 4;
    });

    if (result == kNamespaceUnavailable) {
        QSKIP("Unprivileged user/mount namespaces are not available");
    }
    QCOMPARE(result, 0);
}

void TestFilesystemHelpers::safeRemoveAllStopsAtSameFilesystemBindMount()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // 同一FS内のbind mountはst_devが変わらないため、STATX_ATTR_MOUNT_ROOTで検出する必要がある
    const QString source = tempDir.path() + QStringLiteral("/source");
    const QString tree = tempDir.path() + QStringLiteral("/tree");
    const QString bindPoint = tree + QStringLiteral("/bind");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(bindPoint));
    QVERIFY(createEmptyFile(source + QStringLiteral("/keep")));

    const int result = runInPrivateMountNamespace([&]() {
        if (::mount(source.toUtf8().constData(), bindPoint.toUtf8().constData(),
                    nullptr, MS_BIND, nullptr) < 0) {
            return kNamespaceUnavailable;
        }

        struct statx stx;
        if (::statx(AT_FDCWD, bindPoint.toUtf8().constData(), AT_SYMLINK_NOFOLLOW,
                    STATX_TYPE, &stx) < 0) {
            return 1;
        }
        if ((stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) == 0) {
            // STATX_ATTR_MOUNT_ROOT非対応カーネルではbind mountを検出できない
            return kNamespaceUnavailable;
        }

        errno = 0;
        if (safeRemoveAllBeneathRoot(QStringLiteral("/"), tree)) {
            return 2;
        }
        if (errno != EXDEV) {
            return 3;
        }
        return existsNoFollow(source + QStringLiteral("/keep")) ? 0 : 4;
    });

    if (result == kNamespaceUnavailable) {
        QSKIP("Unprivileged bind mounts or STATX_ATTR_MOUNT_ROOT are not available");
    }
    QCOMPARE(result, 0);
    QVERIFY(QFile::exists(source + QStringLiteral("/keep")));
}

void TestFilesystemHelpers::safeRemoveAllBeneathRootStopsAtMountBoundary()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString root = tempDir.path();
    const QString created = root + QStringLiteral("/created");
    const QString mountPoint = created + QStringLiteral("/nfs");
    QVERIFY(QDir().mkpath(mountPoint));
    QVERIFY(createEmptyFile(created + QStringLiteral("/sibling")));

    const int result = runInPrivateMountNamespace([&]() {
        if (::mount("tmpfs", mountPoint.toUtf8().constData(), "tmpfs", 0, "size=1m") < 0) {
            return kNamespaceUnavailable;
        }
        if (!createEmptyFile(mountPoint + QStringLiteral("/remote"))) {
            return 1;
        }

        errno = 0;
        if (safeRemoveAllBeneathRoot(root, created)) {
            return 2;
        }
        if (errno != EXDEV) {
            return 3;
        }
        return existsNoFollow(mountPoint + QStringLiteral("/remote")) ? 0 : 4;
    });

    if (result == kNamespaceUnavailable) {
        QSKIP("Unprivileged user/mount namespaces are not available");
    }
    QCOMPARE(result, 0);
}

void TestFilesystemHelpers::safeRemoveAllStopsAtNestedBtrfsSubvolume()
{
    // ネストしたsubvolumeはmountではないが、st_devが親と異なる
    // /tmpはtmpfsであることが多いため、HOME配下にbtrfsの作業ディレクトリを作る
    const QString base = QDir::homePath();
    struct statfs fsInfo;
    if (::statfs(base.toUtf8().constData(), &fsInfo) < 0
            || fsInfo.f_type != BTRFS_SUPER_MAGIC) {
        QSKIP("HOME is not on btrfs");
    }

    QTemporaryDir tempDir(base + QStringLiteral("/.qsnapper-test-XXXXXX"));
    QVERIFY(tempDir.isValid());

    const QString tree = tempDir.path() + QStringLiteral("/tree");
    QVERIFY(QDir().mkpath(tree));

    const int treeFd = ::open(tree.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(treeFd >= 0);
    struct btrfs_ioctl_vol_args args;
    std::memset(&args, 0, sizeof(args));
    std::strncpy(args.name, "subvol", sizeof(args.name) - 1);
    const int created = ::ioctl(treeFd, BTRFS_IOC_SUBVOL_CREATE, &args);
    ::close(treeFd);
    if (created < 0) {
        QSKIP("Cannot create a btrfs subvolume as this user");
    }

    const QString subvolume = tree + QStringLiteral("/subvol");
    const QString inside = subvolume + QStringLiteral("/inside");
    QVERIFY(createEmptyFile(inside));

    errno = 0;
    QVERIFY(!safeRemoveAllBeneathRoot(QStringLiteral("/"), tree));
    QCOMPARE(errno, EXDEV);
    QVERIFY(existsNoFollow(inside));

    // 後片付け: 空のsubvolumeは所有者がrmdirできる (kernel 4.18以降)
    QVERIFY(::unlink(inside.toUtf8().constData()) == 0);
    if (::rmdir(subvolume.toUtf8().constData()) < 0) {
        qWarning("Could not remove test subvolume: %s", std::strerror(errno));
    }
}

// ============================================================================
// 復元時のmetadata保持とrename前後の差し替え検出
// ============================================================================

namespace {

    constexpr quint16 kAclUserObj = 0x01;
    constexpr quint16 kAclUser = 0x02;
    constexpr quint16 kAclGroupObj = 0x04;
    constexpr quint16 kAclMask = 0x10;
    constexpr quint16 kAclOther = 0x20;
    constexpr quint32 kAclUndefinedId = 0xffffffffU;

    /**
     * @brief POSIX ACLの1 entry
     */
    struct AclEntry {
        quint16 tag;
        quint16 perm;
        quint32 id;
    };

    /**
     * @brief system.posix_acl_* のxattr値 (version 2、little endian) を組み立てる
     * @param entries tag順・id順に並べたentry
     * @return xattr値
     */
    QByteArray makeAclXattr(const QList<AclEntry> &entries)
    {
        QByteArray value;
        const auto append = [&value](quint64 number, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                value.append(static_cast<char>((number >> (8 * i)) & 0xff));
            }
        };
        append(2, 4);
        for (const AclEntry &entry : entries) {
            append(entry.tag, 2);
            append(entry.perm, 2);
            append(entry.id, 4);
        }
        return value;
    }

    /**
     * @brief named user entryを1件持つACLを返す (mode 0640相当)
     * @param namedUser named entryのuid
     * @return xattr値
     */
    QByteArray namedUserAcl(quint32 namedUser)
    {
        return makeAclXattr({
            {kAclUserObj, 6, kAclUndefinedId},
            {kAclUser, 4, namedUser},
            {kAclGroupObj, 4, kAclUndefinedId},
            {kAclMask, 4, kAclUndefinedId},
            {kAclOther, 0, kAclUndefinedId},
        });
    }

    /**
     * @brief パスのxattr値を読み出す
     * @param path 対象パス
     * @param name xattr名
     * @param valueOut 値の返却先
     * @return 1: 値あり、0: 属性なし、-1: エラー
     */
    int readXattr(const QString &path, const char *name, QByteArray *valueOut)
    {
        char buffer[4096];
        const ssize_t length = ::lgetxattr(path.toUtf8().constData(), name, buffer, sizeof(buffer));
        if (length < 0) {
            return errno == ENODATA ? 0 : -1;
        }
        *valueOut = QByteArray(buffer, static_cast<qsizetype>(length));
        return 1;
    }

    /**
     * @brief パスへxattrを設定する
     * @param path 対象パス
     * @param name xattr名
     * @param value 値
     * @return 成功時0、失敗時errno
     */
    int writeXattr(const QString &path, const char *name, const QByteArray &value)
    {
        if (::lsetxattr(path.toUtf8().constData(), name, value.constData(),
                        static_cast<size_t>(value.size()), 0) < 0) {
            return errno;
        }
        return 0;
    }

    /**
     * @brief ファイルを指定内容・modeで作成する
     * @param path 作成先
     * @param content 内容
     * @param mode 最終的に設定するmode
     * @return 作成できた場合true
     */
    bool createFile(const QString &path, const QByteArray &content, mode_t mode)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || file.write(content) != content.size()) {
            return false;
        }
        file.close();
        return ::chmod(path.toUtf8().constData(), mode) == 0;
    }

    /**
     * @brief 復元元ファイルを開き、宛先を親dirfd + leaf名で差し替える
     * @param root 宛先の基点
     * @param source 復元元パス
     * @param destination 宛先パス
     * @param metadataOut metadataの適用結果
     * @return 差し替えに成功した場合true
     */
    bool replaceFromPath(const QString &root, const QString &source, const QString &destination,
                         RestoredMetadataResult *metadataOut = nullptr)
    {
        const int sourceFd = ::open(source.toUtf8().constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (sourceFd < 0) {
            return false;
        }
        QByteArray leafName;
        const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
        if (parentFd < 0) {
            ::close(sourceFd);
            return false;
        }
        const bool replaced = replaceRegularFileAt(parentFd, leafName, sourceFd, false, true, metadataOut);
        const int savedErrno = errno;
        ::close(parentFd);
        ::close(sourceFd);
        errno = savedErrno;
        return replaced;
    }

    /**
     * @brief ディレクトリ内に一時名 (".qsnapper-") が残っていないか確認する
     * @param directory 対象ディレクトリ
     * @return 残っていなければtrue
     */
    bool hasNoTemporaryLeftovers(const QString &directory)
    {
        const QStringList entries = QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (entry.contains(QStringLiteral(".qsnapper-"))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief rename直前フックをスコープ終了時に解除する
     */
    struct ReplaceHookGuard {
        explicit ReplaceHookGuard(std::function<void(int, const QByteArray &)> hook)
        {
            detail::setBeforeReplaceRenameHookForTesting(std::move(hook));
        }
        ~ReplaceHookGuard()
        {
            detail::setBeforeReplaceRenameHookForTesting({});
        }
        ReplaceHookGuard(const ReplaceHookGuard &) = delete;
        ReplaceHookGuard &operator=(const ReplaceHookGuard &) = delete;
    };

}

void TestFilesystemHelpers::replaceRegularFileAtReplacesContentWithNewInode()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/live/file");
    const QString hardlink = root + QStringLiteral("/live/hardlink");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/live")));
    QVERIFY(createFile(source, "snapshot content", 0640));
    QVERIFY(createFile(destination, "live content that is longer", 0666));
    QVERIFY(::link(destination.toUtf8().constData(), hardlink.toUtf8().constData()) == 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QFile restored(destination);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("snapshot content"));

    // 既存inodeを上書きしないため、hardlink先は元の内容のまま残り、緩いmodeも引き継がない
    QFile other(hardlink);
    QVERIFY(other.open(QIODevice::ReadOnly));
    QCOMPARE(other.readAll(), QByteArray("live content that is longer"));

    struct stat sourceStat;
    struct stat restoredStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
    QCOMPARE(restoredStat.st_mtim.tv_sec, sourceStat.st_mtim.tv_sec);
    QCOMPARE(restoredStat.st_mtim.tv_nsec, sourceStat.st_mtim.tv_nsec);
    QVERIFY(hasNoTemporaryLeftovers(root + QStringLiteral("/live")));
}

void TestFilesystemHelpers::replaceRegularFileAtPreservesSetuidAndSetgid()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(createFile(source, "#!/bin/sh\n", 06755));
    QVERIFY(createFile(destination, "old", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    if ((sourceStat.st_mode & 06000) != 06000) {
        QSKIP("This filesystem or user cannot set setuid/setgid bits");
    }

    // 所有者の変更はS_ISUID / S_ISGIDを落とすため、fchown → fchmodの順でなければ失われる
    QVERIFY(replaceFromPath(root, source, destination));

    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, sourceStat.st_mode & 07777);
    QCOMPARE(restoredStat.st_uid, sourceStat.st_uid);
    QCOMPARE(restoredStat.st_gid, sourceStat.st_gid);
}

void TestFilesystemHelpers::replaceRegularFileAtCopiesAccessAcl()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(createFile(source, "acl", 0640));
    QVERIFY(createFile(destination, "old", 0600));

    const QByteArray acl = namedUserAcl(::getuid() + 1000);
    const int setError = writeXattr(source, "system.posix_acl_access", acl);
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QByteArray sourceAcl;
    QByteArray restoredAcl;
    QCOMPARE(readXattr(source, "system.posix_acl_access", &sourceAcl), 1);
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 1);
    QCOMPARE(restoredAcl, sourceAcl);

    struct stat sourceStat;
    struct stat restoredStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, sourceStat.st_mode & 07777);
}

void TestFilesystemHelpers::replaceRegularFileAtDropsInheritedDefaultAcl()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString liveDir = root + QStringLiteral("/live");
    const QString destination = liveDir + QStringLiteral("/file");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(source, "no acl", 0640));

    // 親ディレクトリのdefault ACLが新しいinodeへ継承されても、snapshot側に無いentryを残さない
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 7, ::getuid() + 1000},
        {kAclGroupObj, 7, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 7, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QByteArray restoredAcl;
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 0);

    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
}

void TestFilesystemHelpers::applyRestoredMetadataSyncsDirectoryAcls()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString withAcl = tempDir.path() + QStringLiteral("/with-acl");
    const QString withoutAcl = tempDir.path() + QStringLiteral("/without-acl");
    QVERIFY(QDir().mkpath(withAcl));
    QVERIFY(QDir().mkpath(withoutAcl));
    QVERIFY(::chmod(withAcl.toUtf8().constData(), 02750) == 0);
    QVERIFY(::chmod(withoutAcl.toUtf8().constData(), 0755) == 0);

    const QByteArray defaultAcl = makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 5, ::getuid() + 1000},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 5, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    });
    const int setError = writeXattr(withAcl, "system.posix_acl_default", defaultAcl);
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);
    QCOMPARE(writeXattr(withAcl, "system.posix_acl_access", namedUserAcl(::getuid() + 1000)), 0);

    const auto apply = [](const QString &from, const QString &to) {
        const int sourceFd = ::open(from.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int destinationFd = ::open(to.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const RestoredMetadataResult result = applyRestoredMetadata(sourceFd, destinationFd, true);
        ::close(sourceFd);
        ::close(destinationFd);
        return result;
    };

    // snapshot側のaccess / default ACLを、ACLを持たない既存ディレクトリへ適用する
    const QString copyTarget = tempDir.path() + QStringLiteral("/copy-target");
    QVERIFY(QDir().mkpath(copyTarget));
    QVERIFY(!apply(withAcl, copyTarget).mandatoryFailed);
    QByteArray value;
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_default", &value), 1);
    QCOMPARE(value, defaultAcl);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_access", &value), 1);

    // snapshot側にACLが無い場合は、live側のaccess / default ACLを削除する
    QVERIFY(!apply(withoutAcl, copyTarget).mandatoryFailed);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_default", &value), 0);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_access", &value), 0);

    struct stat restoredStat;
    QVERIFY(::lstat(copyTarget.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0755U);
}

void TestFilesystemHelpers::replaceRegularFileAtDetectsTemporaryNameSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    const QString stolen = root + QStringLiteral("/stolen");
    QVERIFY(createFile(source, "snapshot", 0644));
    QVERIFY(createFile(destination, "live", 0644));

    // rename直前に一時名を攻撃者のobjectへ差し替える
    const ReplaceHookGuard guard([&](int parentFd, const QByteArray &temporaryName) {
        QVERIFY(::renameat(parentFd, temporaryName.constData(), parentFd, "stolen") == 0);
        const int fd = ::openat(parentFd, temporaryName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        QVERIFY(fd >= 0);
        QVERIFY(::write(fd, "attacker", 8) == 8);
        ::close(fd);
    });

    errno = 0;
    QVERIFY(!replaceFromPath(root, source, destination));
    QCOMPARE(errno, ESTALE);

    // 攻撃者のobjectは宛先へrenameされず、宛先は元のまま
    QFile live(destination);
    QVERIFY(live.open(QIODevice::ReadOnly));
    QCOMPARE(live.readAll(), QByteArray("live"));
    QVERIFY(QFile::exists(stolen));
}

void TestFilesystemHelpers::replaceRegularFileAtKeepsResolvedParentAfterParentSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString parent = root + QStringLiteral("/parent");
    const QString moved = root + QStringLiteral("/moved");
    const QString outside = root + QStringLiteral("/outside");
    QVERIFY(createFile(source, "snapshot", 0644));
    QVERIFY(QDir().mkpath(parent));
    QVERIFY(QDir().mkpath(outside));

    // 解決済みの親をsymlinkへ差し替えても、保持している親dirfd上で完結し、symlink先へは書き込まない
    const ReplaceHookGuard guard([&](int, const QByteArray &) {
        QVERIFY(::rename(parent.toUtf8().constData(), moved.toUtf8().constData()) == 0);
        QVERIFY(::symlink(outside.toUtf8().constData(), parent.toUtf8().constData()) == 0);
    });

    QVERIFY(replaceFromPath(root, source, parent + QStringLiteral("/file")));
    QVERIFY(QFile::exists(moved + QStringLiteral("/file")));
    QVERIFY(!QFile::exists(outside + QStringLiteral("/file")));
    QVERIFY(hasNoTemporaryLeftovers(outside));
}

void TestFilesystemHelpers::replaceSymlinkAtAppliesMetadataAndDetectsSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString sourceLink = root + QStringLiteral("/source-link");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(::symlink("target/path", sourceLink.toUtf8().constData()) == 0);
    const struct timespec times[2] = { {1000000000, 0}, {1100000000, 123} };
    QVERIFY(::utimensat(AT_FDCWD, sourceLink.toUtf8().constData(), times, AT_SYMLINK_NOFOLLOW) == 0);
    QVERIFY(createFile(destination, "live", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(sourceLink.toUtf8().constData(), &sourceStat) == 0);

    QByteArray leafName;
    const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(parentFd >= 0);

    bool metadataApplied = false;
    QVERIFY(replaceSymlinkAt(parentFd, leafName, "target/path", sourceStat, &metadataApplied));
    QVERIFY(metadataApplied);
    QCOMPARE(QFile::symLinkTarget(destination).endsWith(QStringLiteral("target/path")), true);
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QVERIFY(S_ISLNK(restoredStat.st_mode));
    QCOMPARE(restoredStat.st_mtim.tv_sec, sourceStat.st_mtim.tv_sec);
    QCOMPARE(restoredStat.st_mtim.tv_nsec, sourceStat.st_mtim.tv_nsec);

    // rename直前に一時symlinkを別のsymlinkへ差し替えると検出する
    {
        const ReplaceHookGuard guard([&](int hookParentFd, const QByteArray &temporaryName) {
            QVERIFY(::unlinkat(hookParentFd, temporaryName.constData(), 0) == 0);
            QVERIFY(::symlinkat("/etc/shadow", hookParentFd, temporaryName.constData()) == 0);
        });
        errno = 0;
        QVERIFY(!replaceSymlinkAt(parentFd, leafName, "other/target", sourceStat, nullptr));
        QCOMPARE(errno, ESTALE);
    }
    ::close(parentFd);

    char target[64] = {};
    QVERIFY(::readlink(destination.toUtf8().constData(), target, sizeof(target) - 1) > 0);
    QCOMPARE(QByteArray(target), QByteArray("target/path"));
}

// ============================================================================
// beneath-root 宛先解決ハードニング (Todo 2)
// ============================================================================

/**
 * @brief 攻撃検出時に許容するerrnoの集合 (プラットフォーム安全な拒否)
 *
 * ELOOP: symlink差し替え検出 / ENOTDIR: 非ディレクトリ成分 /
 * EXDEV: mount境界 / EACCES: 権限 / EINVAL: 入力解析による拒否 (本実装のmapped error)
 */
bool TestFilesystemHelpers::isAttackRejectionErrno(int err)
{
    switch (err) {
    case ELOOP:
    case ENOTDIR:
    case EXDEV:
    case EACCES:
    case EINVAL:
        return true;
    default:
        return false;
    }
}

/**
 * @brief malformed入力に対する「安全な取り扱い」として許容するerrnoの集合
 */
bool TestFilesystemHelpers::isSafeHandlingErrno(int err)
{
    if (isAttackRejectionErrno(err)) {
        return true;
    }
    switch (err) {
    case ENOENT:
    case EEXIST:
    case EISDIR:
    case ENOTEMPTY:
    case EPERM:
    case ENAMETOOLONG:
        return true;
    default:
        return false;
    }
}

/**
 * @brief ディレクトリ木の内包物 (相対パス, ソート済み) を列挙する
 *
 * symlink自体は追従せずエントリとして記録する
 */
QStringList TestFilesystemHelpers::fingerprintTree(const QString &dirPath)
{
    QStringList entries;
    QDirIterator it(dirPath,
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        entries.append(it.next().mid(static_cast<int>(dirPath.size()) + 1));
    }
    entries.sort();
    return entries;
}

/**
 * @brief 攻撃レイアウトを構築する
 *
 * root/a/b (実ディレクトリ) と outside/b (decoy用) を作り、outside直下に
 * センチネル secret.txt、outside/b に decoy target.txt を配置する。
 * victimPath は凍結済み宛先 root/a/b/target.txt を表す
 */
void TestFilesystemHelpers::buildAttackFixture(QTemporaryDir *rootDir, QTemporaryDir *outsideDir,
                                               QString *victimPath, QByteArray *sentinel) const
{
    QVERIFY(rootDir->isValid());
    QVERIFY(outsideDir->isValid());

    QVERIFY(QDir().mkpath(rootDir->path() + QStringLiteral("/a/b")));
    QVERIFY(QDir().mkpath(outsideDir->path() + QStringLiteral("/b")));

    const QByteArray payload("TOP-SECRET-SENTINEL-42\n");
    QFile sentinelFile(outsideDir->path() + QStringLiteral("/secret.txt"));
    QVERIFY(sentinelFile.open(QIODevice::WriteOnly | QIODevice::Text));
    QCOMPARE(sentinelFile.write(payload.constData(), payload.size()), payload.size());
    sentinelFile.close();

    QFile decoyFile(outsideDir->path() + QStringLiteral("/b/target.txt"));
    QVERIFY(decoyFile.open(QIODevice::WriteOnly | QIODevice::Text));
    decoyFile.write("decoy");
    decoyFile.close();

    *victimPath = rootDir->path() + QStringLiteral("/a/b/target.txt");
    *sentinel = payload;
}

/**
 * @brief rootからの相対成分で指定した実ディレクトリをoutsideへのsymlinkへ差し替える
 *
 * 攻撃者がfreeze後に親成分を置き換える操作を再現する
 */
void TestFilesystemHelpers::swapComponentToSymlink(const QTemporaryDir &rootDir,
                                                   const QTemporaryDir &outsideDir,
                                                   const QStringList &components)
{
    const QString componentPath = rootDir.path() + QLatin1Char('/')
            + components.join(QLatin1Char('/'));
    QVERIFY(QDir(componentPath).exists());
    // 差し替え前の実ディレクトリを攻撃者が削除する操作の再現
    QVERIFY(safeRemoveAllBeneathRoot(QStringLiteral("/"), componentPath));
    QVERIFY(::symlink(outsideDir.path().toUtf8().constData(), componentPath.toUtf8().constData())
            == 0);
}

/**
 * @brief 外部ディレクトリが一切変化していないことを検証する
 *
 * センチネルのbyte一致と、木全体のfingerprint一致を見る
 */
void TestFilesystemHelpers::verifyOutsideUntouched(const QTemporaryDir &outsideDir,
                                                   const QByteArray &sentinel,
                                                   const QStringList &fingerprintBefore) const
{
    QFile sentinelFile(outsideDir.path() + QStringLiteral("/secret.txt"));
    QVERIFY(sentinelFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(sentinelFile.readAll(), sentinel);
    sentinelFile.close();

    QCOMPARE(fingerprintTree(outsideDir.path()), fingerprintBefore);
}

void TestFilesystemHelpers::splitDestinationBeneathRootAcceptsNestedDestination()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path();
    const QString destination = rootPath + QStringLiteral("/a/b/c.txt");

    QString relative;
    QVERIFY(splitDestinationBeneathRoot(rootPath, destination, &relative));
    QCOMPARE(relative, QStringLiteral("a/b/c.txt"));

    // 末尾スラッシュや連続スラッシュは正規化される
    QString normalizedRelative;
    QVERIFY(splitDestinationBeneathRoot(rootPath + QStringLiteral("/"),
                                        rootPath + QStringLiteral("//a///b.txt"),
                                        &normalizedRelative));
    QCOMPARE(normalizedRelative, QStringLiteral("a/b.txt"));

    // root = "/" の場合は全ての絶対パスが配下となる
    QString rootRelative;
    QVERIFY(splitDestinationBeneathRoot(QStringLiteral("/"), QStringLiteral("/etc/passwd"),
                                        &rootRelative));
    QCOMPARE(rootRelative, QStringLiteral("etc/passwd"));
}

void TestFilesystemHelpers::splitDestinationBeneathRootRejectsDotDotTraversal()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString relative;

    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(),
                                         tempDir.path() + QStringLiteral("/../escape.txt"),
                                         &relative));
    QCOMPARE(errno, EINVAL);

    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(),
                                         tempDir.path() + QStringLiteral("/a/../b.txt"),
                                         &relative));
    QCOMPARE(errno, EINVAL);

    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(),
                                         tempDir.path() + QStringLiteral("/.."),
                                         &relative));
    QCOMPARE(errno, EINVAL);
}

void TestFilesystemHelpers::splitDestinationBeneathRootRejectsOutsideRoot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString relative;

    // root配下に全く含まれない絶対パス
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), QStringLiteral("/etc/passwd"),
                                         &relative));
    QCOMPARE(errno, EINVAL);

    // 兄弟ディレクトリtrick (プレフィックスが文字列として似ているだけ)
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), tempDir.path() + QStringLiteral("-evil/x"),
                                         &relative));
    QCOMPARE(errno, EINVAL);

    // root自身は宛先にならない
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), tempDir.path(), &relative));
    QCOMPARE(errno, EINVAL);
}

void TestFilesystemHelpers::splitDestinationBeneathRootRejectsMalformedPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString relative;

    // 空の宛先
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), QString(), &relative));
    QCOMPARE(errno, EINVAL);

    // "/"そのもの
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), QStringLiteral("/"), &relative));
    QCOMPARE(errno, EINVAL);

    // 相対パス (絶対パス要件違反)
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), QStringLiteral("a/b.txt"), &relative));
    QCOMPARE(errno, EINVAL);

    // 空のroot
    QVERIFY(!splitDestinationBeneathRoot(QString(), QStringLiteral("/a/b.txt"), &relative));
    QCOMPARE(errno, EINVAL);

    // 出力先null
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(),
                                         tempDir.path() + QStringLiteral("/a.txt"), nullptr));
    QCOMPARE(errno, EINVAL);

    // 埋め込みNUL (syscall引数の切り詰めにより検証対象と変異対象がズレるため拒否する)
    QString nulDestination = tempDir.path() + QStringLiteral("/a");
    nulDestination.append(QChar(u'\0'));
    nulDestination += QStringLiteral("/b.txt");
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(), nulDestination, &relative));
    QCOMPARE(errno, EINVAL);

    // 制御文字 (C0)
    QVERIFY(!splitDestinationBeneathRoot(tempDir.path(),
                                         tempDir.path() + QStringLiteral("/\u0001x"), &relative));
    QCOMPARE(errno, EINVAL);
}

void TestFilesystemHelpers::safeOpenDirectoryBeneathRootWalksNestedPath()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/x/y/z")));

    const int fd = safeOpenDirectoryBeneathRoot(tempDir.path(), QStringLiteral("x/y/z"), false, 0);
    QVERIFY(fd >= 0);

    struct stat st;
    QCOMPARE(::fstat(fd, &st), 0);
    QVERIFY(S_ISDIR(st.st_mode));
    ::close(fd);
}

void TestFilesystemHelpers::safeOpenDirectoryBeneathRootCreatesMissing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int fd = safeOpenDirectoryBeneathRoot(tempDir.path(), QStringLiteral("p/q/r"), true,
                                                0755);
    QVERIFY(fd >= 0);
    ::close(fd);

    QVERIFY(QDir(tempDir.path() + QStringLiteral("/p/q/r")).exists());
}

void TestFilesystemHelpers::safeOpenDirectoryBeneathRootRejectsSymlinkComponent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString realDir = tempDir.path() + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realDir));

    const QString linkPath = tempDir.path() + QStringLiteral("/link");
    QVERIFY(::symlink(realDir.toUtf8().constData(), linkPath.toUtf8().constData()) == 0);

    errno = 0;
    const int fd = safeOpenDirectoryBeneathRoot(tempDir.path(), QStringLiteral("link/sub"), false,
                                                0);
    const int err = errno;
    QVERIFY(fd < 0);
    QWARN(qPrintable(QStringLiteral("errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));
}

/**
 * @brief 見出しケース: parent成分を外部ディレクトリへのsymlinkへ差し替えた状態での書き込み
 *
 * ヘルパーが失敗すること、および外部センチネルがbyte-for-byte不変であることを検証する
 */
void TestFilesystemHelpers::beneathRootWriteRejectsParentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    // freeze後に攻撃者が即時parent (a/b) を外部へのsymlinkへ差し替える
    swapComponentToSymlink(rootDir, outsideDir,
                           {QStringLiteral("a"), QStringLiteral("b")});

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QWARN(qPrintable(QStringLiteral("write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootMkdirRejectsParentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir,
                           {QStringLiteral("a"), QStringLiteral("b")});

    const QString newDirPath = rootDir.path() + QStringLiteral("/a/b/newdir");
    errno = 0;
    const bool created = safeCreateDirectoryBeneathRoot(rootDir.path(), newDirPath, 0755);
    const int err = errno;
    QVERIFY(!created);
    QWARN(qPrintable(QStringLiteral("mkdir errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootRenameAsideRejectsParentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir,
                           {QStringLiteral("a"), QStringLiteral("b")});

    // rename-asideの退避先もroot配下に凍結されている想定
    const QString asidePath = rootDir.path() + QStringLiteral("/a/b/.target.txt.qsnapper-old");
    errno = 0;
    const bool renamed = safeRenamePathNoFollowBeneathRoot(rootDir.path(), victimPath, asidePath);
    const int err = errno;
    QVERIFY(!renamed);
    QWARN(qPrintable(QStringLiteral("rename errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootRemoveRejectsParentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir,
                           {QStringLiteral("a"), QStringLiteral("b")});

    errno = 0;
    const bool removed = safeRemoveAllBeneathRoot(rootDir.path(), victimPath);
    const int err = errno;
    // symlink差し替え検出時は必ずfalseかつ安全なerrnoとなる
    // (decoyが外部に配置してあるため、追従する実装はここでtrueを返してしまう)
    QVERIFY(!removed);
    QWARN(qPrintable(QStringLiteral("remove errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootSymlinkCreateRejectsParentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir,
                           {QStringLiteral("a"), QStringLiteral("b")});

    const QString linkPath = rootDir.path() + QStringLiteral("/a/b/link");
    errno = 0;
    const bool created = safeCreateSymlinkNoFollowBeneathRoot(rootDir.path(),
                                                              QByteArray("elsewhere"), linkPath);
    const int err = errno;
    QVERIFY(!created);
    QWARN(qPrintable(QStringLiteral("symlink errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

/**
 * @brief symlink差し替えが祖父母階層 (root/a) の場合も検出すること
 */
void TestFilesystemHelpers::beneathRootWriteRejectsGrandparentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir, {QStringLiteral("a")});

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QWARN(qPrintable(QStringLiteral("grandparent write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootMkdirRejectsGrandparentSymlinkSwap()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir, {QStringLiteral("a")});

    const QString newDirPath = rootDir.path() + QStringLiteral("/a/b/newdir");
    errno = 0;
    const bool created = safeCreateDirectoryBeneathRoot(rootDir.path(), newDirPath, 0755);
    const int err = errno;
    QVERIFY(!created);
    QWARN(qPrintable(QStringLiteral("grandparent mkdir errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

/**
 * @brief leaf自身が外部ファイルへのsymlinkへ差し替えられた場合、追従せず拒否する
 */
void TestFilesystemHelpers::beneathRootWriteRejectsLeafSymlinkToExternalFile()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    // leafを外部センチネルへのsymlinkへ差し替える (parentは実ディレクトリのまま)
    QVERIFY(safeRemoveAllBeneathRoot(QStringLiteral("/"), victimPath));
    QVERIFY(::symlink((outsideDir.path() + QStringLiteral("/secret.txt")).toUtf8().constData(),
                      victimPath.toUtf8().constData()) == 0);

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QWARN(qPrintable(QStringLiteral("leaf-symlink write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootWriteRejectsDanglingSymlinkComponent()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    // 中間成分をdangling symlinkへ差し替える
    QVERIFY(safeRemoveAllBeneathRoot(QStringLiteral("/"), rootDir.path() + QStringLiteral("/a")));
    QVERIFY(::symlink(QStringLiteral("/nonexistent/qsnapper-dangling-target").toUtf8().constData(),
                      (rootDir.path() + QStringLiteral("/a")).toUtf8().constData()) == 0);

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QWARN(qPrintable(QStringLiteral("dangling write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

void TestFilesystemHelpers::beneathRootWriteRejectsRegularFileAsDirectoryComponent()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    // 中間成分が通常ファイルの場合はENOTDIRで拒否する
    const QString filePath = rootDir.path() + QStringLiteral("/f.txt");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("plain");
    file.close();

    const QString destination = filePath + QStringLiteral("/nested.txt");
    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), destination, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QCOMPARE(err, ENOTDIR);
}

void TestFilesystemHelpers::beneathRootHappyPathWritesNestedFileWithMode()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    const QString destDir = rootDir.path() + QStringLiteral("/x/y");
    const QString destination = destDir + QStringLiteral("/file.txt");

    QVERIFY(safeCreateDirectoryBeneathRoot(rootDir.path(), destDir, 0755));
    QVERIFY(QDir(destDir).exists());

    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), destination, 0644);
    QVERIFY(fd >= 0);

    const QByteArray payload("restored-content\n");
    QCOMPARE(::write(fd, payload.constData(), static_cast<size_t>(payload.size())),
             payload.size());
    ::close(fd);

    QFile verify(destination);
    QVERIFY(verify.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(verify.readAll(), payload);
    verify.close();

    struct stat st;
    QCOMPARE(::lstat(destination.toUtf8().constData(), &st), 0);
    QVERIFY(S_ISREG(st.st_mode));
    QCOMPARE(st.st_mode & 0777, 0644);
}

void TestFilesystemHelpers::beneathRootRenameAsideMovesWithinRoot()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    QVERIFY(QDir().mkpath(rootDir.path() + QStringLiteral("/a")));
    const QString sourcePath = rootDir.path() + QStringLiteral("/a/live.txt");
    {
        QFile file(sourcePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("live");
    }

    const QString asidePath = rootDir.path() + QStringLiteral("/a/.live.txt.qsnapper-old");
    QVERIFY(safeRenamePathNoFollowBeneathRoot(rootDir.path(), sourcePath, asidePath));
    QVERIFY(!QFile::exists(sourcePath));
    QVERIFY(QFile::exists(asidePath));

    QFile verify(asidePath);
    QVERIFY(verify.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(verify.readAll(), QByteArray("live"));
}

void TestFilesystemHelpers::beneathRootRenameAsideDoesNotReplaceExisting_data()
{
    QTest::addColumn<QString>("existingKind");

    QTest::newRow("regular file") << QStringLiteral("file");
    QTest::newRow("symlink")      << QStringLiteral("symlink");
    QTest::newRow("empty dir")    << QStringLiteral("dir");
}

void TestFilesystemHelpers::beneathRootRenameAsideDoesNotReplaceExisting()
{
    QFETCH(QString, existingKind);

    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    QVERIFY(QDir().mkpath(rootDir.path() + QStringLiteral("/a")));
    const QString sourcePath = rootDir.path() + QStringLiteral("/a/live.txt");
    {
        QFile file(sourcePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("live");
    }

    // 退避先の名前を先に占有しておく (renameat()は通常ファイル・symlink・空ディレクトリを黙って置換する)
    const QString asidePath = rootDir.path() + QStringLiteral("/a/.live.txt.qsnapper-old");
    const QByteArray asideBytes = QFile::encodeName(asidePath);
    if (existingKind == QLatin1String("file")) {
        QFile file(asidePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("occupied");
    }
    else if (existingKind == QLatin1String("symlink")) {
        QCOMPARE(::symlink("/nonexistent-target", asideBytes.constData()), 0);
    }
    else {
        QCOMPARE(::mkdir(asideBytes.constData(), 0700), 0);
    }

    errno = 0;
    const bool renamed = safeRenamePathNoFollowBeneathRoot(rootDir.path(), sourcePath, asidePath);
    const int err = errno;
    QVERIFY(!renamed);
    QCOMPARE(err, EEXIST);

    // 移動元はそのまま残る
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArray("live"));

    // 既存のエントリも置換されていない
    struct stat st {};
    QCOMPARE(::lstat(asideBytes.constData(), &st), 0);
    if (existingKind == QLatin1String("file")) {
        QVERIFY(S_ISREG(st.st_mode));
        QFile aside(asidePath);
        QVERIFY(aside.open(QIODevice::ReadOnly));
        QCOMPARE(aside.readAll(), QByteArray("occupied"));
    }
    else if (existingKind == QLatin1String("symlink")) {
        QVERIFY(S_ISLNK(st.st_mode));
    }
    else {
        QVERIFY(S_ISDIR(st.st_mode));
    }
}

/**
 * @brief 欠けている親ディレクトリはsnapshot側の同じディレクトリのmodeで作成され、snapshot側に無い場合は0700になることを確認する
 */
void TestFilesystemHelpers::beneathRootMissingParentsFollowSnapshotMetadata()
{
    QTemporaryDir liveDir;
    QTemporaryDir snapshotDir;
    QVERIFY(liveDir.isValid() && snapshotDir.isValid());

    // snapshot側: a (0751) / a/b (0710)、a/b/c は存在しない、a/link は別ディレクトリへのsymlink
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("a/b")));
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("elsewhere")));
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a")).constData(), 0751), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a/b")).constData(), 0710), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/elsewhere")).constData(), 0755), 0);
    QCOMPARE(::symlink("../elsewhere",
                       QFile::encodeName(snapshotDir.path() + QStringLiteral("/a/link")).constData()), 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            liveDir.path(), liveDir.path() + QStringLiteral("/a/b/c"), snapshotFd, false);
    const bool createdThroughLink = safeCreateParentDirectoriesFromSourceBeneathRoot(
            liveDir.path(), liveDir.path() + QStringLiteral("/a/link"), snapshotFd, false);
    ::close(snapshotFd);
    QVERIFY(created);
    QVERIFY(createdThroughLink);

    const auto modeOf = [&liveDir](const QString &relativePath) {
        struct stat st {};
        const QByteArray path = QFile::encodeName(liveDir.path() + QLatin1Char('/') + relativePath);
        if (::lstat(path.constData(), &st) < 0 || !S_ISDIR(st.st_mode)) {
            return mode_t(~0);
        }
        return mode_t(st.st_mode & 07777);
    };
    QCOMPARE(modeOf(QStringLiteral("a")), mode_t(0751));
    QCOMPARE(modeOf(QStringLiteral("a/b")), mode_t(0710));
    QCOMPARE(modeOf(QStringLiteral("a/b/c")), mode_t(0700));
    // snapshot側がsymlinkの場合は「無し」と判定し、リンク先のmetadataを適用しない
    QCOMPARE(modeOf(QStringLiteral("a/link")), mode_t(0700));
}

/**
 * @brief 既存の親ディレクトリのmodeは変更されないことを確認する
 */
void TestFilesystemHelpers::beneathRootMissingParentsDoNotModifyExistingParents()
{
    QTemporaryDir liveDir;
    QTemporaryDir snapshotDir;
    QVERIFY(liveDir.isValid() && snapshotDir.isValid());

    QVERIFY(QDir(liveDir.path()).mkpath(QStringLiteral("a")));
    QCOMPARE(::chmod(QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData(), 0750), 0);
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("a/b")));
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a")).constData(), 0777), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a/b")).constData(), 0755), 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            liveDir.path(), liveDir.path() + QStringLiteral("/a/b"), snapshotFd, false);
    ::close(snapshotFd);
    QVERIFY(created);

    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData(), &st), 0);
    QCOMPARE(mode_t(st.st_mode & 07777), mode_t(0750));
    QCOMPARE(::stat(QFile::encodeName(liveDir.path() + QStringLiteral("/a/b")).constData(), &st), 0);
    QCOMPARE(mode_t(st.st_mode & 07777), mode_t(0755));
}

/**
 * @brief live側の途中の成分がsymlinkの場合は失敗し、root外に何も作成しないことを確認する
 */
void TestFilesystemHelpers::beneathRootMissingParentsRejectLiveSymlinkComponent()
{
    QTemporaryDir liveDir;
    QTemporaryDir snapshotDir;
    QTemporaryDir outsideDir;
    QVERIFY(liveDir.isValid() && snapshotDir.isValid() && outsideDir.isValid());

    QCOMPARE(::symlink(QFile::encodeName(outsideDir.path()).constData(),
                       QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData()), 0);
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("a/b")));
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);
    errno = 0;
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            liveDir.path(), liveDir.path() + QStringLiteral("/a/b"), snapshotFd, false);
    const int err = errno;
    ::close(snapshotFd);

    QVERIFY(!created);
    QVERIFY(isAttackRejectionErrno(err));
    QCOMPARE(fingerprintTree(outsideDir.path()), fingerprintBefore);
}

/**
 * @brief .snapshots の指紋が、作成・変更・削除で変わり、変化が無ければ変わらないことを確認する
 */
void TestFilesystemHelpers::snapshotListFingerprintDetectsListChanges()
{
    QTemporaryDir snapshotsDir;
    QVERIFY(snapshotsDir.isValid());
    const QString base = snapshotsDir.path();

    const auto writeInfo = [&base](int number, const QByteArray &content) {
        const QString dirPath = base + QLatin1Char('/') + QString::number(number);
        if (!QDir().mkpath(dirPath)) {
            return false;
        }
        // snapperと同じく、一時ファイルからのrenameで置き換える
        QFile temporary(dirPath + QStringLiteral("/info.xml.tmp"));
        if (!temporary.open(QIODevice::WriteOnly) || temporary.write(content) != content.size()) {
            return false;
        }
        temporary.close();
        return ::rename(QFile::encodeName(temporary.fileName()).constData(),
                        QFile::encodeName(dirPath + QStringLiteral("/info.xml")).constData()) == 0;
    };

    QVERIFY(writeInfo(1, "one"));
    QVERIFY(writeInfo(2, "two"));

    const QByteArray initial = snapshotListFingerprint(base);
    QVERIFY(!initial.isEmpty());
    QCOMPARE(snapshotListFingerprint(base), initial);

    // 作成
    QVERIFY(writeInfo(3, "three"));
    const QByteArray afterCreate = snapshotListFingerprint(base);
    QVERIFY(!afterCreate.isEmpty());
    QVERIFY(afterCreate != initial);

    // 変更 (同じサイズの内容でinfo.xmlを置き換える)
    QVERIFY(writeInfo(2, "TWO"));
    const QByteArray afterModify = snapshotListFingerprint(base);
    QVERIFY(afterModify != afterCreate);

    // 削除
    QVERIFY(QDir(base + QStringLiteral("/1")).removeRecursively());
    const QByteArray afterDelete = snapshotListFingerprint(base);
    QVERIFY(afterDelete != afterModify);

    // 判定できない場合は空を返す
    QVERIFY(snapshotListFingerprint(base + QStringLiteral("/missing")).isEmpty());
}

/**
 * @brief 先頭に0が付いた名前のエントリも指紋の対象になることを確認する
 */
void TestFilesystemHelpers::snapshotListFingerprintDetectsLeadingZeroEntries()
{
    QTemporaryDir snapshotsDir;
    QVERIFY(snapshotsDir.isValid());
    const QString base = snapshotsDir.path();

    QVERIFY(QDir().mkpath(base + QStringLiteral("/01")));
    QFile info(base + QStringLiteral("/01/info.xml"));
    QVERIFY(info.open(QIODevice::WriteOnly));
    QCOMPARE(info.write("one"), qint64(3));
    info.close();

    const QByteArray initial = snapshotListFingerprint(base);
    QVERIFY(!initial.isEmpty());

    // 同じサイズの内容でinfo.xmlを置き換えても指紋が変わる
    QFile replacement(base + QStringLiteral("/01/info.xml.tmp"));
    QVERIFY(replacement.open(QIODevice::WriteOnly));
    QCOMPARE(replacement.write("ONE"), qint64(3));
    replacement.close();
    QCOMPARE(::rename(QFile::encodeName(replacement.fileName()).constData(),
                      QFile::encodeName(base + QStringLiteral("/01/info.xml")).constData()), 0);

    const QByteArray afterModify = snapshotListFingerprint(base);
    QVERIFY(!afterModify.isEmpty());
    QVERIFY(afterModify != initial);
}

/**
 * @brief metadata適用の可否が親ディレクトリの所有者とmodeから判定されることを確認する
 */
void TestFilesystemHelpers::parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode()
{
    const auto makeStat = [](mode_t mode, uid_t uid) {
        struct stat st {};
        st.st_mode = mode | S_IFDIR;
        st.st_uid = uid;
        return st;
    };

    // root所有・他ユーザー書き込み不可は安全
    QVERIFY(parentDirectoryIsSafeForMetadata(makeStat(0755, 0)));
    QVERIFY(parentDirectoryIsSafeForMetadata(makeStat(0700, 0)));
    // sticky bit付きなら、root所有エントリを他ユーザーは差し替えられない
    QVERIFY(parentDirectoryIsSafeForMetadata(makeStat(01777, 0)));
    QVERIFY(parentDirectoryIsSafeForMetadata(makeStat(01755, 0)));
    // sticky bitが無いgroup/other書き込みは差し替えられる
    QVERIFY(!parentDirectoryIsSafeForMetadata(makeStat(0777, 0)));
    QVERIFY(!parentDirectoryIsSafeForMetadata(makeStat(0770, 0)));
    QVERIFY(!parentDirectoryIsSafeForMetadata(makeStat(0775, 0)));
    // 書き込み可能なディレクトリは所有者が差し替えられる
    QVERIFY(!parentDirectoryIsSafeForMetadata(makeStat(0700, 1000)));
    QVERIFY(!parentDirectoryIsSafeForMetadata(makeStat(0755, 1000)));
    // 所有者でも書き込み不可なら差し替えられない
    QVERIFY(parentDirectoryIsSafeForMetadata(makeStat(0555, 1000)));
}

/**
 * @brief metadata適用の可否が実際の作成結果に反映されることを確認する (root専用)
 */
void TestFilesystemHelpers::beneathRootMissingParentsFollowTrustedParentPolicy()
{
    // 信頼判定はeuid 0のときだけ作成結果へ反映されるため、root以外では実行しない
    if (::geteuid() != 0) {
        QSKIP("requires root to exercise the metadata preservation policy");
    }

    QTemporaryDir snapshotDir;
    QVERIFY(snapshotDir.isValid());
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("a")));
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a")).constData(), 0751), 0);
    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    // 信頼できる親 (root所有・他ユーザー書き込み不可) ではsnapshot側のmodeを適用する
    {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        QVERIFY(safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/a"), snapshotFd, true));
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData(), &st), 0);
        QCOMPARE(mode_t(st.st_mode & 07777), mode_t(0751));
        QCOMPARE(st.st_uid, ::geteuid());
    }

    // 他ユーザーが書き込める親ではmetadataを適用せず、他ユーザー書き込み不可の0711で作成する
    {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        QCOMPARE(::chown(QFile::encodeName(liveDir.path()).constData(), 65534, 65534), 0);
        QCOMPARE(::chmod(QFile::encodeName(liveDir.path()).constData(), 0700), 0);

        const mode_t previousUmask = ::umask(0);
        const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/a"), snapshotFd, true);
        ::umask(previousUmask);
        QVERIFY(created);

        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData(), &st), 0);
        QCOMPARE(mode_t(st.st_mode & 07777), mode_t(0711));
        QCOMPARE(st.st_uid, ::geteuid());
    }

    ::close(snapshotFd);
}

void TestFilesystemHelpers::beneathRootSymlinkHelpersApplyWithinRoot()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    QVERIFY(QDir().mkpath(rootDir.path() + QStringLiteral("/sub")));

    const QByteArray target("target-value");
    const QString linkPath = rootDir.path() + QStringLiteral("/sub/link");
    QVERIFY(safeCreateSymlinkNoFollowBeneathRoot(rootDir.path(), target, linkPath));

    char observedTarget[64] = {};
    QVERIFY(::readlink(linkPath.toUtf8().constData(), observedTarget, sizeof(observedTarget) - 1) > 0);
    QCOMPARE(QByteArray(observedTarget), target);

    struct stat st;
    QVERIFY(::lstat(linkPath.toUtf8().constData(), &st) == 0);
    struct timespec ts[2];
    ts[0] = st.st_atim;
    ts[1] = st.st_mtim;
    bool ownerUpdated = false;
    bool timesUpdated = false;
    QVERIFY(safeSetSymlinkMetadataNoFollowBeneathRoot(rootDir.path(), linkPath, ::getuid(),
                                                      ::getgid(), ts, &ownerUpdated,
                                                      &timesUpdated));
    QVERIFY(ownerUpdated);
    QVERIFY(timesUpdated);

    // root外の宛先は拒否される
    const QString outsideLink = QStringLiteral("/tmp/qsnapper-t2-should-not-exist-link");
    QVERIFY(!safeCreateSymlinkNoFollowBeneathRoot(rootDir.path(), target, outsideLink));
    QVERIFY(!safeSetSymlinkMetadataNoFollowBeneathRoot(rootDir.path(), outsideLink, ::getuid(),
                                                       ::getgid(), ts, &ownerUpdated,
                                                       &timesUpdated));
    QVERIFY(!QFile::exists(outsideLink));
}

/**
 * @brief stale state: 解決後に成分を差し替えても変異が失敗すること
 *
 * freeze時の文字列解決をどれだけ信頼しても、実行時の再解決が攻撃を捕捉する
 */
void TestFilesystemHelpers::beneathRootStaleResolutionStillRejected()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    // freeze時点の解決 (この時点では正当なパス)
    QString relative;
    QVERIFY(splitDestinationBeneathRoot(rootDir.path(), victimPath, &relative));
    QCOMPARE(relative, QStringLiteral("a/b/target.txt"));

    // その後に攻撃者が差し替える
    swapComponentToSymlink(rootDir, outsideDir, {QStringLiteral("a")});

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    if (fd >= 0) {
        ::close(fd);
    }
    QWARN(qPrintable(QStringLiteral("stale write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

/**
 * @brief 攻撃50回連続でもdescriptor漏洩が無いこと (/proc/self/fdで計測)
 */
void TestFilesystemHelpers::beneathRootRepeatedSwapsDoNotLeakDescriptors()
{
    QTemporaryDir rootDir;
    QTemporaryDir outsideDir;
    QVERIFY(rootDir.isValid() && outsideDir.isValid());

    QString victimPath;
    QByteArray sentinel;
    buildAttackFixture(&rootDir, &outsideDir, &victimPath, &sentinel);
    const QStringList fingerprintBefore = fingerprintTree(outsideDir.path());

    swapComponentToSymlink(rootDir, outsideDir, {QStringLiteral("a")});

    const auto countOpenFds = []() {
        DIR *dir = ::opendir("/proc/self/fd");
        if (!dir) {
            return -1;
        }
        int count = 0;
        while (::readdir(dir) != nullptr) {
            ++count;
        }
        ::closedir(dir);
        return count - 2; // "." と ".." 分を除く
    };

    const int beforeRejected = countOpenFds();
    QVERIFY(beforeRejected >= 0);

    for (int i = 0; i < 50; ++i) {
        errno = 0;
        const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), victimPath, 0644);
        const int err = errno;
        QVERIFY(fd < 0);
        QVERIFY(isAttackRejectionErrno(err));
    }

    const int afterRejected = countOpenFds();
    QVERIFY(afterRejected >= 0);
    QCOMPARE(afterRejected, beforeRejected);

    // 成功路径でも漏洩しないこと
    QVERIFY(QDir().mkpath(rootDir.path() + QStringLiteral("/ok")));
    const int beforeSuccess = countOpenFds();
    QVERIFY(beforeSuccess >= 0);
    for (int i = 0; i < 10; ++i) {
        const QString okPath = rootDir.path() + QStringLiteral("/ok/loop%1.txt").arg(i);
        const int fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), okPath, 0644);
        QVERIFY(fd >= 0);
        QCOMPARE(::write(fd, "x", 1), 1);
        ::close(fd);
    }
    const int afterSuccess = countOpenFds();
    QVERIFY(afterSuccess >= 0);
    QCOMPARE(afterSuccess, beforeSuccess);

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

/**
 * @brief malformed入力がクラッシュやroot外書き込みにならないこと
 */
void TestFilesystemHelpers::beneathRootHandlesMalformedInputsSafely()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    // 空の宛先 / "/" / root自身
    QVERIFY(safeOpenRegularFileWriteBeneathRoot(rootDir.path(), QString(), 0644) < 0);
    QCOMPARE(errno, EINVAL);
    QVERIFY(safeOpenRegularFileWriteBeneathRoot(rootDir.path(), QStringLiteral("/"), 0644) < 0);
    QCOMPARE(errno, EINVAL);
    QVERIFY(safeOpenRegularFileWriteBeneathRoot(rootDir.path(), rootDir.path(), 0644) < 0);
    QCOMPARE(errno, EINVAL);

    // 連続スラッシュは正規化されてroot配下に収まる (write系は親を作らないため先に作成)
    const QString messyDir = rootDir.path() + QStringLiteral("//messy///dir");
    QVERIFY(safeCreateDirectoryBeneathRoot(rootDir.path(), messyDir, 0755));
    QVERIFY(QDir(rootDir.path() + QStringLiteral("/messy/dir")).exists());

    const QString messyPath = messyDir + QStringLiteral("/file.txt");
    const int messyFd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), messyPath, 0644);
    QVERIFY(messyFd >= 0);
    ::close(messyFd);
    QVERIFY(QFile::exists(rootDir.path() + QStringLiteral("/messy/dir/file.txt")));

    // 非常に深いネスト: kernel上限に当たっても安全に失敗、またはroot配下で成功する
    QString deepRelative;
    for (int i = 0; i < 400; ++i) {
        deepRelative += QStringLiteral("d%1/").arg(i % 10);
    }
    deepRelative += QStringLiteral("leaf.txt");
    const int deepFd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), deepRelative, 0644);
    if (deepFd >= 0) {
        ::close(deepFd);
        QVERIFY(QFile::exists(rootDir.path() + QLatin1Char('/') + deepRelative));
    }
    else {
        QVERIFY(isSafeHandlingErrno(errno));
    }

    // 非UTF8バイト列はQString化時に置換文字へ正規化され、root配下で安全に扱われる
    const QString nonUtf8 = QString::fromUtf8(QByteArray("\xff\xfe\x80" "bad"));
    const QString nonUtf8Destination = rootDir.path() + QLatin1Char('/') + nonUtf8;
    const int nonUtf8Fd = safeOpenRegularFileWriteBeneathRoot(rootDir.path(), nonUtf8Destination,
                                                              0644);
    if (nonUtf8Fd >= 0) {
        ::close(nonUtf8Fd);
        QVERIFY(QFile::exists(nonUtf8Destination));
    }
    else {
        QVERIFY(isSafeHandlingErrno(errno));
    }

    // 埋め込みNULを含む宛先はwrapperレベルでも拒否する (切り詰れpathへの変異を防ぐ)
    QString nulDestination = rootDir.path() + QStringLiteral("/a");
    nulDestination.append(QChar(u'\0'));
    nulDestination += QStringLiteral("/b.txt");
    QVERIFY(safeCreateDirectoryBeneathRoot(rootDir.path(),
                                           rootDir.path() + QStringLiteral("/a"), 0755));
    errno = 0;
    QVERIFY(safeOpenRegularFileWriteBeneathRoot(rootDir.path(), nulDestination, 0644) < 0);
    QCOMPARE(errno, EINVAL);
}

void TestFilesystemHelpers::beneathRootRejectsSymlinkInRootPathAncestry()
{
    // rootPathそのものの中間成分がsymlink化されている場合も追従せず拒否する
    // (rootの末尾成分だけでなく祖先成分の差し替えも防御対象)
    QTemporaryDir baseDir;
    QTemporaryDir evilDir;
    QVERIFY(baseDir.isValid() && evilDir.isValid());

    // base/link -> evilDir であり、rootPath = base/link/root は実ディレクトリ
    QVERIFY(QDir().mkpath(evilDir.path() + QStringLiteral("/root")));
    QVERIFY(::symlink(evilDir.path().toUtf8().constData(),
                      (baseDir.path() + QStringLiteral("/link")).toUtf8().constData()) == 0);

    const QString swappedRoot = baseDir.path() + QStringLiteral("/link/root");
    const QString evilRootDir = evilDir.path() + QStringLiteral("/root");

    // O_NOFOLLOW walk は ENOTDIR、openat2(RESOLVE_NO_SYMLINKS) は ELOOP を返す。
    // どちらもsymlink祖先を追従せず拒否する同じセキュリティ契約である。
    errno = 0;
    QVERIFY(!safeCreateDirectoryBeneathRoot(swappedRoot,
                                            swappedRoot + QStringLiteral("/child"), 0755));
    QVERIFY(isAttackRejectionErrno(errno));

    // 差し替え先ディレクトリには何も作成されていない
    QVERIFY(QDir(evilRootDir).entryList(QDir::AllEntries | QDir::Hidden
                                        | QDir::NoDotAndDotDot).isEmpty());

    errno = 0;
    QVERIFY(safeOpenRegularFileWriteBeneathRoot(swappedRoot,
                                                swappedRoot + QStringLiteral("/f.txt"),
                                                0644) < 0);
    QVERIFY(isAttackRejectionErrno(errno));
    QVERIFY(QDir(evilRootDir).entryList(QDir::AllEntries | QDir::Hidden
                                        | QDir::NoDotAndDotDot).isEmpty());
}

void TestFilesystemHelpers::safeLstatAtResolvesRelativeToDirFd()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/sub")));
    const QString sourceFile = tempDir.path() + QStringLiteral("/sub/source.txt");
    {
        QFile file(sourceFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("pinned");
    }

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    // dirfd相対で解決される
    struct stat st;
    QVERIFY(safeLstatAt(dirFd, QStringLiteral("sub/source.txt"), &st));
    QVERIFY(S_ISREG(st.st_mode));

    // leafのsymlinkは展開せずsymlink自身を報告する
    const QString leafLink = tempDir.path() + QStringLiteral("/leaf-link");
    QVERIFY(::symlink(QStringLiteral("sub/source.txt").toUtf8().constData(),
                      leafLink.toUtf8().constData()) == 0);
    struct stat linkSt;
    QVERIFY(safeLstatAt(dirFd, QStringLiteral("leaf-link"), &linkSt));
    QVERIFY(S_ISLNK(linkSt.st_mode));

    QVERIFY(::close(dirFd) == 0);
}

void TestFilesystemHelpers::safeLstatAtRejectsAbsoluteAndTraversalPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    struct stat st;
    // 絶対パスはdirfdを無視して解決されるため入力段階で拒否する
    QVERIFY(!safeLstatAt(dirFd, QStringLiteral("/etc/passwd"), &st));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeLstatAt(dirFd, QStringLiteral("../escape"), &st));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeLstatAt(dirFd, QStringLiteral("a/./b"), &st));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeLstatAt(dirFd, QStringLiteral("a/../b"), &st));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeLstatAt(dirFd, QStringLiteral("trick\u0000x"), &st));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeLstatAt(dirFd, QString(), &st));
    QCOMPARE(errno, EINVAL);

    QVERIFY(::close(dirFd) == 0);
}

void TestFilesystemHelpers::safeOpenRegularFileReadAtResolvesRelativeToDirFd()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourceFile = tempDir.path() + QStringLiteral("/source.txt");
    {
        QFile file(sourceFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("data");
    }

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    const int fd = safeOpenRegularFileReadAt(dirFd, QStringLiteral("source.txt"));
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("data"));
    }

    QVERIFY(::close(dirFd) == 0);
}

void TestFilesystemHelpers::safeOpenRegularFileReadAtRejectsSymlinkLeafAndNonRegular()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourceFile = tempDir.path() + QStringLiteral("/source.txt");
    {
        QFile file(sourceFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("data");
    }

    const QString linkPath = tempDir.path() + QStringLiteral("/link.txt");
    QVERIFY(::symlink(sourceFile.toUtf8().constData(), linkPath.toUtf8().constData()) == 0);
    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/subdir")));

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    // leafのsymlinkはO_NOFOLLOWで拒否される
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("link.txt")) < 0);
    QCOMPARE(errno, ELOOP);

    // directoryは通常ファイルとして開けない
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("subdir")) < 0);

    // 絶対パス・".."/"."成分・制御文字は検証段階で拒否される
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, sourceFile) < 0);
    QCOMPARE(errno, EINVAL);
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("a/../b.txt")) < 0);
    QCOMPARE(errno, EINVAL);
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("a\u0000b.txt")) < 0);
    QCOMPARE(errno, EINVAL);

    QVERIFY(::close(dirFd) == 0);
}

void TestFilesystemHelpers::safeReadLinkNoFollowAtReadsRelativeToDirFd()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/sub")));
    const QByteArray target("sub/actual-target");
    const QString linkPath = tempDir.path() + QStringLiteral("/sub/link");
    QVERIFY(::symlink(target.constData(), linkPath.toUtf8().constData()) == 0);

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    QByteArray observedTarget;
    QVERIFY(safeReadLinkNoFollowAt(dirFd, QStringLiteral("sub/link"), &observedTarget));
    QCOMPARE(observedTarget, target);

    QVERIFY(::close(dirFd) == 0);
}

void TestFilesystemHelpers::safeReadLinkNoFollowAtRejectsInvalidPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/sub")));

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    QByteArray observedTarget;
    // 絶対パス・ ".."・"."・空・制御文字は検証段階で拒否される
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("/etc/passwd"), &observedTarget));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("../link"), &observedTarget));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("./link"), &observedTarget));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QString(), &observedTarget));
    QCOMPARE(errno, EINVAL);
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("sub/li\u0000nk"), &observedTarget));
    QCOMPARE(errno, EINVAL);

    // symlink以外のleafはreadlinkatとして失敗する
    errno = 0;
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("sub"), &observedTarget));
    QCOMPARE(errno, EINVAL);

    QVERIFY(::close(dirFd) == 0);
}

/**
 * @brief 復元元snapshotに存在するパスを不在と誤判定しないことを検証する
 *
 * 復元の `created` entryはlive側からの削除として実行されるため、
 * ここでtrueを返してしまうと復元元に存在するパスが削除される
 */
void TestFilesystemHelpers::isConfirmedAbsentAtRejectsPathsPresentInSource()
{
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());

    QVERIFY(QDir().mkpath(sourceDir.path() + QStringLiteral("/etc")));
    const QString presentFile = sourceDir.path() + QStringLiteral("/etc/important");
    {
        QFile file(presentFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("victim");
    }
    const QString danglingLink = sourceDir.path() + QStringLiteral("/etc/dangling");
    QVERIFY(::symlink("missing-target", danglingLink.toUtf8().constData()) == 0);

    const int dirFd = ::open(sourceDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/important")));

    // leafがdangling symlinkでも、symlink自体は存在するので不在とはみなさない
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/dangling")));

    QVERIFY(::close(dirFd) == 0);
}

/**
 * @brief 不在を確定できた場合だけtrueを返すことを検証する
 *
 * 「不在」と「確認できなかった」を混同すると fail-closed が成立しない
 */
void TestFilesystemHelpers::isConfirmedAbsentAtConfirmsOnlyDefiniteAbsence()
{
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());

    QVERIFY(QDir().mkpath(sourceDir.path() + QStringLiteral("/etc")));
    const QString blockingFile = sourceDir.path() + QStringLiteral("/blocking");
    {
        QFile file(blockingFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("not a directory");
    }

    const int dirFd = ::open(sourceDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    // ENOENT: 正当な created entry はここを通って削除へ進む
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("etc/created-after-snapshot")));
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("absent-top-level")));

    // ENOTDIR: 親成分が通常ファイルなので、配下は確定的に存在し得ない
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("blocking/child")));

    // 相対パス検証で弾かれる入力は EINVAL であり「不在を確認できなかった」に当たる
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("/etc/important")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("../escape")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/../etc/important")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/car\nrier")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QString()));

    // 無効なdirfdでも不在とは判定しない
    QVERIFY(!isConfirmedAbsentAt(-1, QStringLiteral("etc/created-after-snapshot")));

    QVERIFY(::close(dirFd) == 0);
}

/**
 * @brief 中間symlinkを辿って不在判定しないことを検証する
 *
 * fstatat()のAT_SYMLINK_NOFOLLOWはleafにしか効かないため、中間成分の解決を
 * fstatat()に任せるとpin済みdirfdの外へ解決され得る。symlinkに当たった場合は
 * 「不在を確認できなかった」として削除を許可してはならない
 */
void TestFilesystemHelpers::isConfirmedAbsentAtRefusesIntermediateSymlinks()
{
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());

    QVERIFY(QDir().mkpath(sourceDir.path() + QStringLiteral("/etc")));

    // 相対target / 絶対target / 存在するディレクトリへのtarget の3種を用意する
    const QString danglingLink = sourceDir.path() + QStringLiteral("/etc/dangling");
    QVERIFY(::symlink("missing-dir", danglingLink.toUtf8().constData()) == 0);
    const QString absoluteLink = sourceDir.path() + QStringLiteral("/etc/absolute");
    QVERIFY(::symlink("/", absoluteLink.toUtf8().constData()) == 0);
    QVERIFY(QDir().mkpath(sourceDir.path() + QStringLiteral("/etc/real-dir")));
    const QString realLink = sourceDir.path() + QStringLiteral("/etc/real-link");
    QVERIFY(::symlink("real-dir", realLink.toUtf8().constData()) == 0);

    const int dirFd = ::open(sourceDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    // いずれも不在判定としてはfalse (不在を確認できていない) でなければならない
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/dangling/victim")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/absolute/etc/passwd")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/real-link/child")));

    // leafのsymlink自体は「存在する」扱いとなる
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/dangling")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/absolute")));
    QVERIFY(!isConfirmedAbsentAt(dirFd, QStringLiteral("etc/real-link")));

    // 比較用: 中間成分が実在しない場合は確定的な不在としてtrue
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("etc/no-such-dir/child")));

    QVERIFY(::close(dirFd) == 0);
}

// ============================================================================
// dirfd相対ソース解決の中間成分in-rootハードニング (RESOLVE_IN_ROOT)
// ============================================================================

/**
 * @brief 絶対symlinkの中間成分がdirfdの外へ解決されないことを検証する
 *
 * 絶対symlink targetと同一の絶対位置をdirfd内に再現し、
 * 中身の一致 (拒否ではなくin-root解決であること) まで確認する
 */
void TestFilesystemHelpers::safeOpenRegularFileReadAtKeepsAbsoluteIntermediateSymlinkInsideRoot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    const QString outsidePath = tempDir.path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(rootPath));
    QVERIFY(QDir().mkpath(outsidePath));

    {
        QFile file(outsidePath + QStringLiteral("/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("OUTSIDE");
    }

    // root内の絶対symlink
    QVERIFY(::symlink(outsidePath.toUtf8().constData(),
                      (rootPath + QStringLiteral("/escape")).toUtf8().constData()) == 0);

    // symlink targetと同じ絶対位置をroot内に再現する (in-root解決ならこちらを見る)
    const QString insideRelative = outsidePath.mid(1);
    QVERIFY(QDir().mkpath(rootPath + QLatin1Char('/') + insideRelative));
    {
        QFile file(rootPath + QLatin1Char('/') + insideRelative
                   + QStringLiteral("/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("INSIDE");
    }

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("escape/secret.txt"));
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("INSIDE"));
    }

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief symlink target内の相対".."がrootでclampされることを検証する
 *
 * RESOLVE_IN_ROOTでは"/.." が"/"に留まるのと同じく、
 * root直下のsymlinkが".."を含んでもdirfdの外へ出ない
 */
void TestFilesystemHelpers::safeOpenRegularFileReadAtClampsDotDotInSymlinkTargetAtRoot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    const QString outsideOfRoot = tempDir.path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/outside")));
    QVERIFY(QDir().mkpath(outsideOfRoot));

    {
        QFile file(outsideOfRoot + QStringLiteral("/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("OUTSIDE");
    }
    {
        QFile file(rootPath + QStringLiteral("/outside/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("INSIDE");
    }

    QVERIFY(::symlink(QStringLiteral("../outside").toUtf8().constData(),
                      (rootPath + QStringLiteral("/escape-rel")).toUtf8().constData()) == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("escape-rel/secret.txt"));
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("INSIDE"));
    }

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief 正当な相対中間symlinkに対する3ヘルパーの後方互換性を検証する
 *
 * snapshot内の "bin -> usr/bin" のような相対symlinkは従来通り解決される
 */
void TestFilesystemHelpers::safeReadHelpersKeepLegitimateRelativeIntermediateSymlinkWorking()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/usr/bin")));
    {
        QFile file(rootPath + QStringLiteral("/usr/bin/tool"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("tool-content");
    }
    QVERIFY(::symlink(QStringLiteral("usr/bin").toUtf8().constData(),
                      (rootPath + QStringLiteral("/bin")).toUtf8().constData()) == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("bin/tool"));
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("tool-content"));
    }

    struct stat st;
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("bin/tool"), &st));
    QVERIFY(S_ISREG(st.st_mode));

    QByteArray target;
    QVERIFY(safeReadLinkNoFollowAt(rootFd, QStringLiteral("bin"), &target));
    QCOMPARE(target, QByteArray("usr/bin"));

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief snapshotに典型的な絶対中間symlinkがin-root解決されることを検証する
 *
 * これはRESOLVE_IN_ROOTポリシーである。snapshotは"/"のbtrfs snapshotであり、
 * "var/run -> /run" はsnapshot内の/runへ解決されるべき正当なsymlinkである
 * (RESOLVE_BENEATHではこのような正当なsnapshotから復元できなくなってしまうため採用していない)
 */
void TestFilesystemHelpers::safeOpenRegularFileReadAtResolvesSnapshotAbsoluteSymlinkInRoot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/var")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/run")));
    {
        QFile file(rootPath + QStringLiteral("/run/file"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("in-root");
    }
    QVERIFY(::symlink(QStringLiteral("/run").toUtf8().constData(),
                      (rootPath + QStringLiteral("/var/run")).toUtf8().constData()) == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("var/run/file"));
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("in-root"));
    }

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief 中間symlinkを通った場合でもleaf symlinkは追従されないことを検証する
 *
 * 復元ロジックはsafeLstatAtがleaf symlinkをsymlinkとして報告することに依存する
 */
void TestFilesystemHelpers::safeHelpersKeepLeafSymlinkUnresolvedThroughIntermediateSymlink()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/usr/bin")));
    {
        QFile file(rootPath + QStringLiteral("/usr/bin/tool"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("tool-content");
    }
    QVERIFY(::symlink(QStringLiteral("usr/bin").toUtf8().constData(),
                      (rootPath + QStringLiteral("/bin")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("tool").toUtf8().constData(),
                      (rootPath + QStringLiteral("/usr/bin/leaflink")).toUtf8().constData())
            == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    // leafはsymlink自体として報告される
    struct stat st;
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("bin/leaflink"), &st));
    QVERIFY(S_ISLNK(st.st_mode));

    // leaf symlinkは通常ファイルとして開けない
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(rootFd, QStringLiteral("bin/leaflink")) < 0);
    QCOMPARE(errno, ELOOP);

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief 中間symlink (絶対 / 相対) を含む正当なsnapshotから復元読み取りできることを検証する
 *
 * 実際のsnapshot構成を模した木に対し、3つの読み取りヘルパーが
 * snapshot自身の内容 / メタデータを返すことを通しで確認する
 */
void TestFilesystemHelpers::restoreReadHelpersHandleSnapshotWithIntermediateSymlinks()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/snapshot");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/etc")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/usr/bin")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/usr/share")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/var")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/run")));

    {
        QFile file(rootPath + QStringLiteral("/etc/motd"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("snapshot motd");
    }
    {
        QFile file(rootPath + QStringLiteral("/run/file"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("in-root run");
    }
    {
        QFile file(rootPath + QStringLiteral("/usr/bin/tool"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("tool-content");
    }

    // snapshot内の絶対symlinkと相対symlink
    QVERIFY(::symlink(QStringLiteral("/run").toUtf8().constData(),
                      (rootPath + QStringLiteral("/var/run")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("usr/bin").toUtf8().constData(),
                      (rootPath + QStringLiteral("/bin")).toUtf8().constData()) == 0);
    // leaf symlink
    QVERIFY(::symlink(QStringLiteral("tool").toUtf8().constData(),
                      (rootPath + QStringLiteral("/usr/bin/tool-link")).toUtf8().constData())
            == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    // 通常ファイルの復元読み取り
    struct stat st;
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("etc/motd"), &st));
    QVERIFY(S_ISREG(st.st_mode));
    {
        const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("etc/motd"));
        QVERIFY(fd >= 0);
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("snapshot motd"));
    }

    // ディレクトリの復元読み取り
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("usr/share"), &st));
    QVERIFY(S_ISDIR(st.st_mode));

    // 絶対中間symlink経由 (RESOLVE_IN_ROOTによるsnapshot内解決)
    {
        const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("var/run/file"));
        QVERIFY(fd >= 0);
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("in-root run"));
    }

    // 相対中間symlink経由
    {
        const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("bin/tool"));
        QVERIFY(fd >= 0);
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("tool-content"));
    }

    // 中間symlink経由でもleaf symlinkはsymlinkとして読める
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("bin/tool-link"), &st));
    QVERIFY(S_ISLNK(st.st_mode));
    QByteArray target;
    QVERIFY(safeReadLinkNoFollowAt(rootFd, QStringLiteral("bin/tool-link"), &target));
    QCOMPARE(target, QByteArray("tool"));

    // 中間symlink自体のtarget取得
    QVERIFY(safeReadLinkNoFollowAt(rootFd, QStringLiteral("bin"), &target));
    QCOMPARE(target, QByteArray("usr/bin"));

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief openat2非対応カーネル向けフォールバックがopenat2と同じ親を解決することを検証する
 *
 * フォールバックは対応カーネル上では到達しないため、detail入口で明示的に選択して検証する
 * 解決先の同一性はst_dev / st_inoで突き合わせる
 */
void TestFilesystemHelpers::inRootFallbackResolvesIdenticallyToOpenat2()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/usr/bin")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/run")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/var")));
    QVERIFY(QDir().mkpath(rootPath + QStringLiteral("/outside")));
    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/outside")));

    {
        QFile file(rootPath + QStringLiteral("/usr/bin/tool"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("tool-content");
    }
    {
        QFile file(rootPath + QStringLiteral("/run/file"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("in-root run");
    }
    {
        QFile file(rootPath + QStringLiteral("/outside/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("INSIDE");
    }
    {
        QFile file(tempDir.path() + QStringLiteral("/outside/secret.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("OUTSIDE");
    }

    QVERIFY(::symlink(QStringLiteral("usr/bin").toUtf8().constData(),
                      (rootPath + QStringLiteral("/bin")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("/run").toUtf8().constData(),
                      (rootPath + QStringLiteral("/var/run")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("../outside").toUtf8().constData(),
                      (rootPath + QStringLiteral("/escape-rel")).toUtf8().constData()) == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    const QStringList paths{
        QStringLiteral("usr/bin/tool"),
        QStringLiteral("bin/tool"),
        QStringLiteral("var/run/file"),
        QStringLiteral("escape-rel/secret.txt")
    };

    for (const QString &path : paths) {
        QByteArray kernelLeaf;
        const int kernelFd = detail::openParentDirectoryInRootForTesting(
                rootFd, path, &kernelLeaf, /*forceNoOpenat2=*/false);
        QVERIFY2(kernelFd >= 0, qPrintable(path));

        QByteArray fallbackLeaf;
        const int fallbackFd = detail::openParentDirectoryInRootForTesting(
                rootFd, path, &fallbackLeaf, /*forceNoOpenat2=*/true);
        QVERIFY2(fallbackFd >= 0, qPrintable(path));

        QCOMPARE(fallbackLeaf, kernelLeaf);

        struct stat kernelStat;
        struct stat fallbackStat;
        QVERIFY(::fstat(kernelFd, &kernelStat) == 0);
        QVERIFY(::fstat(fallbackFd, &fallbackStat) == 0);
        QCOMPARE(fallbackStat.st_dev, kernelStat.st_dev);
        QCOMPARE(fallbackStat.st_ino, kernelStat.st_ino);

        const int leafFd = ::openat(fallbackFd, fallbackLeaf.constData(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        QVERIFY2(leafFd >= 0, qPrintable(path));
        QVERIFY(::close(leafFd) == 0);

        QVERIFY(::close(kernelFd) == 0);
        QVERIFY(::close(fallbackFd) == 0);
    }

    // rootの外にある同名ファイルではなく、root内のファイルが読まれることを直接確認する
    QByteArray leafName;
    const int parentFd = detail::openParentDirectoryInRootForTesting(
            rootFd, QStringLiteral("escape-rel/secret.txt"), &leafName,
            /*forceNoOpenat2=*/true);
    QVERIFY(parentFd >= 0);
    const int fd = ::openat(parentFd, leafName.constData(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    QVERIFY(fd >= 0);
    {
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("INSIDE"));
    }
    QVERIFY(::close(parentFd) == 0);

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief フォールバックがsymlinkループを有限回で打ち切ることを検証する
 *
 * openat2経路と同じくELOOPで失敗し、無限ループにならないこと
 */
void TestFilesystemHelpers::inRootFallbackRejectsSymlinkLoop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath));

    QVERIFY(::symlink(QStringLiteral("loop-b").toUtf8().constData(),
                      (rootPath + QStringLiteral("/loop-a")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("loop-a").toUtf8().constData(),
                      (rootPath + QStringLiteral("/loop-b")).toUtf8().constData()) == 0);

    const int rootFd = ::open(rootPath.toUtf8().constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    errno = 0;
    QVERIFY(detail::openParentDirectoryInRootForTesting(
                rootFd, QStringLiteral("loop-a/child"), nullptr,
                /*forceNoOpenat2=*/true) < 0);
    QCOMPARE(errno, ELOOP);

    errno = 0;
    QVERIFY(detail::openParentDirectoryInRootForTesting(
                rootFd, QStringLiteral("loop-a/child"), nullptr,
                /*forceNoOpenat2=*/false) < 0);
    QCOMPARE(errno, ELOOP);

    QVERIFY(::close(rootFd) == 0);
}

QTEST_APPLESS_MAIN(TestFilesystemHelpers)
#include "tst_filesystemhelpers.moc"
