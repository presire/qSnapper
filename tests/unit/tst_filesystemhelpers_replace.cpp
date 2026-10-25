/**
 * @file tst_filesystemhelpers_replace.cpp
 * @brief 差し替えとmetadata適用の検証
 *
 * 一時object経由の原子的差し替えとmetadata適用、差し替え検出の検証を集める
 */
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

#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;

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

/**
 * @brief 所有者と時刻の適用を必須にした場合、適用できなければlive側を差し替えず、一時symlinkも残さないことを検証する
 *
 * rootで動作する復元では、所有者を保てないsymlinkへ差し替えると別ユーザ所有のlinkが残るため失敗として扱う
 * 一般ユーザはroot所有へ変更できないため、その失敗の経路を再現できる
 */
void TestFilesystemHelpers::replaceSymlinkAtRequiringMetadataLeavesLiveUntouched()
{
    if (::geteuid() == 0) {
        QSKIP("root can always change the link owner");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString sourceLink = root + QStringLiteral("/source-link");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(::symlink("target/path", sourceLink.toUtf8().constData()) == 0);
    QVERIFY(createFile(destination, "live", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(sourceLink.toUtf8().constData(), &sourceStat) == 0);
    sourceStat.st_uid = 0;
    sourceStat.st_gid = 0;

    QByteArray leafName;
    const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(parentFd >= 0);

    bool metadataApplied = true;
    errno = 0;
    QVERIFY(!replaceSymlinkAt(parentFd, leafName, "target/path", sourceStat, &metadataApplied, true));
    QVERIFY(errno != 0);
    QVERIFY(!metadataApplied);
    ::close(parentFd);

    struct stat liveStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &liveStat) == 0);
    QVERIFY(S_ISREG(liveStat.st_mode));
    QVERIFY(hasNoTemporaryLeftovers(root));

    // 必須にしない場合は、従来どおり所有者を保てなくても差し替える
    const int retryParentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(retryParentFd >= 0);
    QVERIFY(replaceSymlinkAt(retryParentFd, leafName, "target/path", sourceStat, &metadataApplied, false));
    QVERIFY(!metadataApplied);
    ::close(retryParentFd);
    QVERIFY(::lstat(destination.toUtf8().constData(), &liveStat) == 0);
    QVERIFY(S_ISLNK(liveStat.st_mode));
}

/**
 * @brief SELinuxコンテキストのユーザ部分だけが復元先の値に置き換わることを確認する
 *
 * qsnapper_dbus_tはSELinuxユーザを変更できないため、拒否されたコンテキストはユーザだけを合わせて書き直す
 * アクセス制御に効くタイプとレベルが変わらないこと、末尾のNULの有無が保たれること、
 * 置き換える意味がない場合と形式が不正な場合に空を返すことを確かめる
 */
void TestFilesystemHelpers::selinuxContextUserIsReplacedOnlyInUserPart()
{
    const QByteArray snapshotContext("unconfined_u:object_r:shadow_t:s0", 33);
    QCOMPARE(selinuxContextWithDestinationUser(snapshotContext, QByteArray("system_u:object_r:etc_t:s0")),
             QByteArray("system_u:object_r:shadow_t:s0"));

    // xattrの値に多い末尾のNULは、スナップショット側の形に合わせて保つ
    const QByteArray terminated = snapshotContext + QByteArray(1, '\0');
    const QByteArray destinationTerminated = QByteArray("system_u:object_r:etc_t:s0") + QByteArray(1, '\0');
    QCOMPARE(selinuxContextWithDestinationUser(terminated, destinationTerminated),
             QByteArray("system_u:object_r:shadow_t:s0") + QByteArray(1, '\0'));
    QCOMPARE(selinuxContextWithDestinationUser(snapshotContext, destinationTerminated),
             QByteArray("system_u:object_r:shadow_t:s0"));

    // MCSのカテゴリに含まれるコロンも、ユーザ以外の部分としてそのまま保つ
    QCOMPARE(selinuxContextWithDestinationUser(QByteArray("staff_u:object_r:user_home_t:s0-s0:c0.c1023"),
                                               QByteArray("system_u:object_r:default_t:s0")),
             QByteArray("system_u:object_r:user_home_t:s0-s0:c0.c1023"));

    // ユーザが同じなら再試行しても結果は変わらないため、空を返す
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("system_u:object_r:etc_t:s0"),
                                              QByteArray("system_u:object_r:var_t:s0")).isEmpty());

    // 形式が不正な値は書き込みに使わない
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("shadow_t"),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray(":object_r:shadow_t:s0"),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(snapshotContext, QByteArray("system_u")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("unconfined_u:object_r:\0shadow_t:s0", 34),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray(), QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
}
