/**
 * @file tst_filesystemhelpers_beneathroot2.cpp
 * @brief beneath-root宛先解決の検証(後半)
 *
 * 親ディレクトリ復元と指紋、信頼親方針と異常系入力の検証を集める
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

    // O_NOFOLLOW walk は ENOTDIR、openat2(RESOLVE_NO_SYMLINKS) は ELOOP を返す
    // どちらもsymlink祖先を追従せず拒否する同じセキュリティ契約である
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
