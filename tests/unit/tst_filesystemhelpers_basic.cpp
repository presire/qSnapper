/**
 * @file tst_filesystemhelpers_basic.cpp
 * @brief 絶対パス起点の安全なオープン・mkpath・再帰削除とmount境界の検証
 *
 * safeMkpath、safeOpenRegularFileRead、safeOpenDirectory、safeLstat、safeRemoveAllBeneathRootを
 * 正規のパスとsymlink攻撃の両方で呼び、拒否と受付の境界を確認する
 *
 * 再帰削除はmount境界を越えてはならないため、ユーザ名前空間とmount名前空間を作る子プロセスで
 * bind mountとmount pointを用意し、削除が境界を越えずに止まることを確認する
 *
 * 利用する側 : tst_filesystemhelpers.hで宣言し、QTEST_APPLESS_MAINを持つtst_filesystemhelpers.cppから実行する
 * 依存 : filesystemhelpers.h(qsnapper::security)、Qt Test、Linuxのmountとbtrfs ioctl、実ファイルシステム
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cerrno>
#include <cstring>
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
#include <unistd.h>
#include "filesystemhelpers.h"
#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;

/**
 * @brief 存在しない親ディレクトリを再帰的に作成できることを確認する
 *
 * 1回の呼び出しでa/b/cまで作り、呼び出し側が階層ごとに作成しなくてよいことを見る
 */
void TestFilesystemHelpers::safeMkpathCreatesNestedDirectories()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString nestedPath = tempDir.path() + QStringLiteral("/a/b/c");
    QVERIFY(safeMkpath(nestedPath));
    QVERIFY(QDir(nestedPath).exists());
}

/**
 * @brief 途中のsymlink成分を辿らず、その先へディレクトリを作らないことを確認する
 *
 * symlinkを辿ると、攻撃者が用意した復元先の外へディレクトリを作らされる
 */
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

/**
 * @brief 作成先そのものがsymlinkの場合に拒否し、リンク先をディレクトリとして扱わないことを確認する
 */
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

/**
 * @brief symlinkの通常ファイルを開かず、リンク先の内容を読ませないことを確認する
 *
 * 正規の通常ファイルは開けることも同時に確認し、拒否がsymlinkだけによるものだと示す
 */
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

/**
 * @brief symlinkを含むディレクトリパスを開かないことを確認する
 *
 * 復元先のwalkは開いたディレクトリを起点に進むため、symlinkで外へ誘導されると以降の操作が全て外部へ向く
 */
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

/**
 * @brief 2048バイトの長いリンク先も切り詰めずに取得することを確認する
 *
 * 固定長のbufferでreadlinkの結果を受ける実装では、長いリンク先が途中で切れて比較を誤る
 */
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

/**
 * @brief symlinkを辿らず、リンク自体の属性を返すことを確認する
 *
 * リンク先の属性を返すと、種別の判定と所有者の確認が別のファイルを指してしまう
 */
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

/**
 * @brief symlinkを含む木を、symlinkを辿らずに削除できることを確認する
 *
 * 木の中のsymlinkを追従すると、復元先の外にあるファイルを削除してしまう
 */
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

    QVERIFY(safeRemoveAllBeneathRoot(tempDir.path(), treePath));
    QVERIFY(!QDir(treePath).exists());

    QFile verify(outsideFile);
    QVERIFY(verify.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(verify.readAll(), QByteArray("outside"));
}

/**
 * @brief 木ではなく単一ファイルを指定した場合も削除できることを確認する
 *
 * 復元計画はファイル単位の削除も行うため、ディレクトリ専用の削除経路に閉じない
 */
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

    QVERIFY(safeRemoveAllBeneathRoot(tempDir.path(), filePath));
    QVERIFY(!QFile::exists(filePath));
}

/**
 * @brief 途中の成分がsymlinkの場合、その先を削除せず拒否することを確認する
 *
 * 攻撃者が親ディレクトリを外部へのsymlinkへ差し替えても、復元先の外を削除させない
 */
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
    QVERIFY(!safeRemoveAllBeneathRoot(tempDir.path(), victimPath));
    QVERIFY(QFile::exists(realFile));
}

// ============================================================================
// 再帰削除のmount境界検出
// ============================================================================

namespace {

    constexpr int kNamespaceUnavailable = 77;   // ユーザー名前空間やmountを作れない環境
    constexpr int kAncestorsNotRootOwned = 78;  // 祖先がroot所有でなく、mount境界の判定まで届かない環境

    /**
     * @brief 文字列をファイルへ書き込む(子プロセス内でも使えるよう、Qtを使わない)
     * @param path 書き込み先
     * @param text 書き込む内容
     * @return 書き込めた場合true
     *
     * @note 既存ファイルだけを対象とし、新規作成はしない
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
     * @brief 非特権のユーザー名前空間とmount名前空間を持つ子プロセスでbodyを実行する
     *
     * root権限なしでmountを作るため、子プロセスでunshare(CLONE_NEWUSER | CLONE_NEWNS)し
     * 自分のuidとgidをnamespace内のrootへ対応付ける
     * 子プロセスの終了とともにmountも消えるため、テスト後の後片付けが不要になる
     *
     * @param body 子プロセスで実行する検証処理(0は成功、それ以外は失敗箇所を示すコード)
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
     * @brief パスの祖先が全てrootの所有で、他者が書き込めないディレクトリかを調べる
     *
     * rootとして行う削除は、ルートから削除の基点までの祖先に、他者が名前を変更できるディレクトリがあるとEPERMで拒否される
     * ユーザー名前空間の中では、ホストのrootが所有する/や/tmpがoverflowuidの所有に見えるため、この前提を満たさない
     * 前提を満たさない環境では、mount境界の判定より前に拒否されるため、境界の挙動を検証できない
     *
     * @param path 削除の基点(基点自身は調べない)
     * @return 全ての祖先がuid 0の所有で、グループと他者に書き込み権限を与えていない場合true
     */
    bool ancestorsAreRootOwned(const QString &path)
    {
        const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString ancestor = QStringLiteral("/");
        for (qsizetype index = 0; index < components.size(); ++index) {
            struct stat info;
            if (::lstat(QFile::encodeName(ancestor).constData(), &info) != 0
                    || info.st_uid != 0
                    || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
                return false;
            }
            if (!ancestor.endsWith(QLatin1Char('/'))) {
                ancestor += QLatin1Char('/');
            }
            ancestor += components.at(index);
        }
        return true;
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
     * @brief symlinkを辿らずにパスの存在を確認する
     * @param path 確認対象
     * @return 存在する場合true
     */
    bool existsNoFollow(const QString &path)
    {
        struct stat st;
        return ::lstat(path.toUtf8().constData(), &st) == 0;
    }

}

/**
 * @brief 木の中のmount pointで再帰削除を止め、境界の内側を消さないことを確認する
 *
 * 別ファイルシステムの内容はスナップショットに含まれず復元もできないため、削除すると取り返しがつかない
 * mountの作成には特権が必要なためprivate mount namespaceで再現し、使えない環境ではQSKIPする
 */
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
        // 祖先の検査で先にEPERMとなる環境では、mount境界の判定まで届かない
        if (!ancestorsAreRootOwned(tempDir.path())) {
            return kAncestorsNotRootOwned;
        }
        if (::mount("tmpfs", mountPoint.toUtf8().constData(), "tmpfs", 0, "size=1m") < 0) {
            return kNamespaceUnavailable;
        }
        if (::mkdir((mountPoint + QStringLiteral("/data")).toUtf8().constData(), 0755) < 0
                || !createEmptyFile(mountPoint + QStringLiteral("/data/inside"))
                || !createEmptyFile(mountPoint + QStringLiteral("/top"))) {
            return 1;
        }

        errno = 0;
        if (safeRemoveAllBeneathRoot(tempDir.path(), tree)) {
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
        // 境界の外側にある親ディレクトリは残る(中身を失ったmount pointの上位を消さない)
        if (!existsNoFollow(tree)) {
            return 5;
        }
        return 0;
    });

    if (result == kNamespaceUnavailable) {
        QSKIP("Unprivileged user/mount namespaces are not available");
    }
    if (result == kAncestorsNotRootOwned) {
        QSKIP("Ancestors of the temporary directory are not root-owned inside the user namespace");
    }
    QCOMPARE(result, 0);
}

/**
 * @brief 削除対象そのものがmount pointの場合も、中身を消さずに拒否することを確認する
 *
 * 削除対象がmount pointなら、配下はスナップショットに含まれない別ファイルシステムである
 */
void TestFilesystemHelpers::safeRemoveAllRefusesMountPointTarget()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString mountPoint = tempDir.path() + QStringLiteral("/mnt");
    QVERIFY(QDir().mkpath(mountPoint));

    const int result = runInPrivateMountNamespace([&]() {
        // 祖先の検査で先にEPERMとなる環境では、mount境界の判定まで届かない
        if (!ancestorsAreRootOwned(tempDir.path())) {
            return kAncestorsNotRootOwned;
        }
        if (::mount("tmpfs", mountPoint.toUtf8().constData(), "tmpfs", 0, "size=1m") < 0) {
            return kNamespaceUnavailable;
        }
        if (!createEmptyFile(mountPoint + QStringLiteral("/inside"))) {
            return 1;
        }

        // 削除対象そのものがmount pointである場合も降りない
        errno = 0;
        if (safeRemoveAllBeneathRoot(tempDir.path(), mountPoint)) {
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
    if (result == kAncestorsNotRootOwned) {
        QSKIP("Ancestors of the temporary directory are not root-owned inside the user namespace");
    }
    QCOMPARE(result, 0);
}

/**
 * @brief 同じファイルシステムのbind mountでも、st_devが同じでも越えないことを確認する
 *
 * bind mountはst_devが親と一致するため、st_devの比較だけでは境界を検出できない
 * そのためSTATX_ATTR_MOUNT_ROOTでmountのrootを判定できることを見る
 */
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
        // 祖先の検査で先にEPERMとなる環境では、mount境界の判定まで届かない
        if (!ancestorsAreRootOwned(tempDir.path())) {
            return kAncestorsNotRootOwned;
        }
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
        if (safeRemoveAllBeneathRoot(tempDir.path(), tree)) {
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
    if (result == kAncestorsNotRootOwned) {
        QSKIP("Ancestors of the temporary directory are not root-owned inside the user namespace");
    }
    QCOMPARE(result, 0);
    QVERIFY(QFile::exists(source + QStringLiteral("/keep")));
}

/**
 * @brief ネストしたbtrfs subvolumeを、mountでなくても境界として扱うことを確認する
 *
 * subvolumeはst_devが親と異なるがmountではないため、mount一覧だけの判定では越えてしまう
 * subvolumeの作成はbtrfsと権限を要するため、作れない環境ではQSKIPする
 */
void TestFilesystemHelpers::safeRemoveAllStopsAtNestedBtrfsSubvolume()
{
    // ネストしたsubvolumeはmountではないが、st_devが親と異なる
    // /tmpはtmpfsであることが多いため、btrfsの作業ディレクトリをHOME配下に作る
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
    QVERIFY(!safeRemoveAllBeneathRoot(tempDir.path(), tree));
    QCOMPARE(errno, EXDEV);
    QVERIFY(existsNoFollow(inside));

    // 後片付け: 空のsubvolumeは所有者がrmdirできる(kernel 4.18以降)
    QVERIFY(::unlink(inside.toUtf8().constData()) == 0);
    if (::rmdir(subvolume.toUtf8().constData()) < 0) {
        qWarning("Could not remove test subvolume: %s", std::strerror(errno));
    }
}
