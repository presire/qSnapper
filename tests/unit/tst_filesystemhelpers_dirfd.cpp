/**
 * @file tst_filesystemhelpers_dirfd.cpp
 * @brief dirfd相対解決の検証
 *
 * dirfd相対のstat・読出・不在確認とin-root解決、フォールバック経路の検証を集める
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
 * 復元のcreated entryはlive側からの削除として実行されるため、
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
 * fstatat()に任せるとpin済みdirfdの外へ解決され得る
 * symlinkに当たった場合は
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
 * これはRESOLVE_IN_ROOTポリシーである
 * snapshotは"/"のbtrfs snapshotであり、
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
