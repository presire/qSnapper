/**
 * @file tst_filesystemhelpers_beneathroot2.cpp
 * @brief beneath-root宛先解決の検証(後半)
 *
 * 親ディレクトリ復元と指紋、信頼親方針と異常系入力の検証を集める
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filesystemhelpers.h"

#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;

/** @brief トップと降下先の権限境界を正常系と同じ入口で検証する行を提供する */
void TestFilesystemHelpers::beneathRootRemovalGuardsDirectoryOwnership_data()
{
    QTest::addColumn<int>("parentUid");
    QTest::addColumn<int>("parentMode");
    QTest::addColumn<int>("targetUid");
    QTest::addColumn<bool>("regular");
    QTest::addColumn<bool>("protectedChild");
    QTest::addColumn<int>("middleUid");
    QTest::addColumn<bool>("allowed");
    QTest::newRow("a-foreign-top") << 1000 << 0755 << 0 << false << false << -1 << false;
    QTest::newRow("b-principal-tree-root-file") << 1000 << 0755 << 1000 << false << false << -1 << true;
    QTest::newRow("c-trusted-tree") << 0 << 0755 << 0 << false << false << -1 << true;
    QTest::newRow("d-root-regular-file") << 1000 << 0755 << 0 << true << false << -1 << true;
    QTest::newRow("e-root-shared-parent") << 0 << 0777 << 1000 << false << false << -1 << false;
    QTest::newRow("f-foreign-descendant") << 1000 << 0755 << 1000 << false << true << -1 << false;
    // 親までが信頼できても、削除対象やその内部で初めて非信頼になる場合は、そこから所有者を限定する
    QTest::newRow("i-user-target-foreign-descendant") << 0 << 0755 << 1000 << false << true << -1 << false;
    QTest::newRow("j-user-target-own-tree") << 0 << 0755 << 1000 << false << false << -1 << true;
    QTest::newRow("k-user-middle-foreign-descendant") << 0 << 0755 << 0 << false << true << 1000 << false;
}

/** @brief 非信頼チェーンの所有者以外のディレクトリは中身も属性も変えずに拒否する */
void TestFilesystemHelpers::beneathRootRemovalGuardsDirectoryOwnership()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to exercise recursive removal ownership guards");
    }
    QFETCH(int, parentUid);
    QFETCH(int, parentMode);
    QFETCH(int, targetUid);
    QFETCH(bool, regular);
    QFETCH(bool, protectedChild);
    QFETCH(int, middleUid);
    QFETCH(bool, allowed);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QCOMPARE(::chmod(QFile::encodeName(root.path()).constData(), 0755), 0);
    const QString parent = root.filePath(QStringLiteral("alice"));
    const QString target = parent + QStringLiteral("/victim");
    QVERIFY(QDir().mkpath(regular ? parent : target));
    QCOMPARE(::chown(QFile::encodeName(parent).constData(), parentUid, parentUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(parent).constData(), parentMode), 0);
    // middleUid指定時は、削除対象とprotectedの間に別の所有者のディレクトリを挟む
    const QString container = middleUid >= 0 ? target + QStringLiteral("/middle") : target;
    const QString protectedPath = protectedChild ? container + QStringLiteral("/protected") : target;
    if (protectedChild) {
        QVERIFY(QDir().mkpath(protectedPath));
        QCOMPARE(::chmod(QFile::encodeName(protectedPath).constData(), 0700), 0);
    }
    if (middleUid >= 0) {
        QCOMPARE(::chown(QFile::encodeName(container).constData(), middleUid, middleUid), 0);
    }
    const QString payload = regular ? target : protectedPath + QStringLiteral("/file");
    {
        QFile file(payload);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("preserved content"), qint64(17));
    }
    QCOMPARE(::chown(QFile::encodeName(target).constData(), targetUid, targetUid), 0);
    struct stat before {};
    QCOMPARE(::lstat(QFile::encodeName(protectedPath).constData(), &before), 0);
    errno = 0;
    const bool removed = safeRemoveAllBeneathRoot(root.path(), target);
    const int removalError = errno;
    QCOMPARE(removed, allowed);
    if (allowed) {
        QVERIFY(!QFileInfo::exists(target));
    }
    else {
        QCOMPARE(removalError, EPERM);
        struct stat after {};
        QCOMPARE(::lstat(QFile::encodeName(protectedPath).constData(), &after), 0);
        QCOMPARE(after.st_dev, before.st_dev);
        QCOMPARE(after.st_ino, before.st_ino);
        QCOMPARE(after.st_uid, before.st_uid);
        QCOMPARE(after.st_mode, before.st_mode);
        QFile file(payload);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("preserved content"));
    }
}

namespace {
    /**
     * @brief パスの所有者とmodeをsymlinkを辿らずに取得する
     *
     * 攻撃でsymlinkや別種別へ差し替えられていないことを検証するため、statではなくlstatを使う
     *
     * @param path 対象パス
     * @param uidOut 所有者の返却先
     * @param modeOut 07777で掩蔽したmodeの返却先
     * @return 対象がディレクトリとして存在する場合true
     */
    bool lstatDirectoryOwnerAndMode(const QString &path, uid_t *uidOut, mode_t *modeOut)
    {
        struct stat st {};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            return false;
        }
        *uidOut = st.st_uid;
        *modeOut = static_cast<mode_t>(st.st_mode & 07777);
        return true;
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
 * @brief sticky bitと所有者のmode変更権限による過信を検出する判定表を用意する
 */
void TestFilesystemHelpers::parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("uid");
    QTest::addColumn<bool>("trusted");
    QTest::newRow("root-0755-trusted") << 0755 << 0 << true;
    QTest::newRow("root-0700-trusted") << 0700 << 0 << true;
    QTest::newRow("root-01755-trusted") << 01755 << 0 << true;
    QTest::newRow("root-0775-untrusted") << 0775 << 0 << false;
    QTest::newRow("root-0777-untrusted") << 0777 << 0 << false;
    QTest::newRow("root-0770-untrusted") << 0770 << 0 << false;
    QTest::newRow("root-01777-untrusted") << 01777 << 0 << false;
    QTest::newRow("user-0700-untrusted") << 0700 << 1000 << false;
    QTest::newRow("user-0755-untrusted") << 0755 << 1000 << false;
    QTest::newRow("user-0555-untrusted") << 0555 << 1000 << false;
}

/**
 * @brief root所有かつgroup/other書き込み不可だけを信頼することを確認する
 */
void TestFilesystemHelpers::parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode()
{
    QFETCH(int, mode);
    QFETCH(int, uid);
    QFETCH(bool, trusted);
    struct stat st {};
    st.st_mode = static_cast<mode_t>(mode) | S_IFDIR;
    st.st_uid = static_cast<uid_t>(uid);
    QCOMPARE(parentDirectoryIsSafeForMetadata(st), trusted);
}

/**
 * @brief metadata適用の可否が実際の作成結果に反映されることを確認する (root専用)
 *
 * 信頼チェーンではsnapshot側の所有者とmodeを適用する
 * 他ユーザー所有の親では復元元の所有者をprincipalと確定できないため作成せず、木を変更しない
 * 同じ処理をprincipal所有の親で行うと成功し、作成物がprincipalの所有物になる
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
        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(liveDir.path() + QStringLiteral("/a"), &uid, &mode));
        QCOMPARE(mode, mode_t(0751));
        QCOMPARE(uid, ::geteuid());
    }

    // 他ユーザー所有の親では、復元元の所有者がその親の所有者と異なるため作成しない
    {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        QCOMPARE(::chown(QFile::encodeName(liveDir.path()).constData(), 65534, 65534), 0);
        QCOMPARE(::chmod(QFile::encodeName(liveDir.path()).constData(), 0755), 0);

        errno = 0;
        const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/a"), snapshotFd, true);
        const int createError = errno;
        QVERIFY(!created);
        QCOMPARE(createError, EPERM);

        // 失敗時に親の所有者とmodeを変えず、作成物も残していないことを確認する
        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(liveDir.path(), &uid, &mode));
        QCOMPARE(uid, uid_t(65534));
        QCOMPARE(mode, mode_t(0755));
        struct stat st {};
        QCOMPARE(::lstat(QFile::encodeName(liveDir.path() + QStringLiteral("/a")).constData(), &st), -1);
        QCOMPARE(errno, ENOENT);
    }

    // 復元元と同じprincipal (uid 1000) 所有の親では、作成物をprincipalの所有物として作成しmetadataを適用する
    {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        const QString principalParent = liveDir.path() + QStringLiteral("/owner");
        QVERIFY(QDir().mkpath(principalParent));
        QCOMPARE(::chown(QFile::encodeName(principalParent).constData(), 1000, 1000), 0);
        QCOMPARE(::chmod(QFile::encodeName(principalParent).constData(), 0755), 0);
        // 復元元の木も同じ相対パス (owner/a) に揃え、所有者をprincipalにする
        QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("owner/a")));
        const QString sourceA = snapshotDir.path() + QStringLiteral("/owner/a");
        QCOMPARE(::chmod(QFile::encodeName(sourceA).constData(), 0751), 0);
        QCOMPARE(::chown(QFile::encodeName(sourceA).constData(), 1000, 1000), 0);

        QVERIFY(safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), principalParent + QStringLiteral("/a"), snapshotFd, true));
        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(principalParent + QStringLiteral("/a"), &uid, &mode));
        QCOMPARE(uid, uid_t(1000));
        QCOMPARE(mode, mode_t(0751));
    }

    ::close(snapshotFd);
}

/**
 * @brief 非信頼な親では作成を拒否し、被験ユーザー所有の親ではsnapshot側のmodeで作成することを確認する
 *
 * 非root呼び出しの0711検索フォールバックは計画で削除が必須のため、非rootでも所有者を確定できる親でのmetadata適用を検証する
 * 0777の親のように所有者を限定できない場所の拒否はrootのときだけ働くため、その検証だけroot専用とする
 */
void TestFilesystemHelpers::beneathRootUntrustedParentKeepsSnapshotSearchRestriction()
{
    // rootで実行する場合はprincipalをuid 1000とし、非rootでは実行ユーザー自身を所有者の基準にする
    const bool runningAsRoot = ::geteuid() == 0;
    const uid_t testUser = runningAsRoot ? uid_t(1000) : ::geteuid();

    QTemporaryDir snapshotDir;
    QVERIFY(snapshotDir.isValid());
    const QString snapshotPath = snapshotDir.path();
    QVERIFY(QDir(snapshotPath).mkpath(QStringLiteral("secret")));
    QVERIFY(QDir(snapshotPath).mkpath(QStringLiteral("public")));
    {
        QFile child(snapshotPath + QStringLiteral("/secret/child"));
        QVERIFY(child.open(QIODevice::WriteOnly));
        QCOMPARE(child.write("hidden"), qint64(6));
    }
    QCOMPARE(::chmod(QFile::encodeName(snapshotPath + QStringLiteral("/secret/child")).constData(), 0644), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotPath + QStringLiteral("/secret")).constData(), 0700), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotPath + QStringLiteral("/public")).constData(), 0755), 0);
    // 復元元の所有者を被験ユーザーに揃える (所有者不一致は別のtestで検証する)
    if (runningAsRoot) {
        QCOMPARE(::chown(QFile::encodeName(snapshotPath + QStringLiteral("/secret")).constData(), testUser, testUser), 0);
        QCOMPARE(::chown(QFile::encodeName(snapshotPath + QStringLiteral("/public")).constData(), testUser, testUser), 0);
    }

    const int snapshotFd = ::open(QFile::encodeName(snapshotPath).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    // 被験ユーザー所有の親では、非rootでもsnapshot側のmodeをそのまま適用して作成する
    {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        if (runningAsRoot) {
            QCOMPARE(::chown(QFile::encodeName(liveDir.path()).constData(), testUser, testUser), 0);
        }
        QCOMPARE(::chmod(QFile::encodeName(liveDir.path()).constData(), 0755), 0);

        QVERIFY(safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/secret"), snapshotFd, true));
        QVERIFY(safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/public"), snapshotFd, true));

        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(liveDir.path() + QStringLiteral("/secret"), &uid, &mode));
        QCOMPARE(uid, testUser);
        QCOMPARE(mode, mode_t(0700));
        QVERIFY(lstatDirectoryOwnerAndMode(liveDir.path() + QStringLiteral("/public"), &uid, &mode));
        QCOMPARE(uid, testUser);
        QCOMPARE(mode, mode_t(0755));
    }

    // 誰でも書き込める親はrootでも所有者を限定できないため、作成せずEPERMで失敗する
    if (runningAsRoot) {
        QTemporaryDir liveDir;
        QVERIFY(liveDir.isValid());
        QCOMPARE(::chmod(QFile::encodeName(liveDir.path()).constData(), 0777), 0);

        errno = 0;
        const bool secretCreated = safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/secret"), snapshotFd, true);
        const int createError = errno;
        QVERIFY(!secretCreated);
        QCOMPARE(createError, EPERM);

        // 親の所有者とmodeを変えず、子を作っていないことを確認する
        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(liveDir.path(), &uid, &mode));
        QCOMPARE(uid, ::geteuid());
        QCOMPARE(mode, mode_t(0777));
        struct stat st {};
        QCOMPARE(::lstat(QFile::encodeName(liveDir.path() + QStringLiteral("/secret")).constData(), &st), -1);
        QCOMPARE(errno, ENOENT);
    }

    ::close(snapshotFd);
}

/**
 * @brief (a) 信頼チェーンでは欠けた親を復元元の所有者とmodeで作成する
 *
 * root所有で他ユーザー書き込み不可のhomeの下に、uid 1000所有のaliceが欠けている場合
 * 従来どおりsnapshot側の所有者とmodeを適用する
 */
void TestFilesystemHelpers::beneathRootTrustedChainCreatesParentWithSourceMetadata()
{
    // 信頼チェーンの作成結果はeuid 0のときだけ検証できるため、root以外では実行しない
    if (::geteuid() != 0) {
        QSKIP("requires root to exercise the trusted chain policy");
    }

    QTemporaryDir rootDir;
    QTemporaryDir snapshotDir;
    QVERIFY(rootDir.isValid() && snapshotDir.isValid());

    QVERIFY(QDir(rootDir.path()).mkpath(QStringLiteral("home")));
    QCOMPARE(::chmod(QFile::encodeName(rootDir.path() + QStringLiteral("/home")).constData(), 0755), 0);

    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("home/alice")));
    const QString sourceAlice = snapshotDir.path() + QStringLiteral("/home/alice");
    QCOMPARE(::chmod(QFile::encodeName(sourceAlice).constData(), 0750), 0);
    QCOMPARE(::chown(QFile::encodeName(sourceAlice).constData(), 1000, 1000), 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    errno = 0;
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            rootDir.path(), rootDir.path() + QStringLiteral("/home/alice"), snapshotFd, true);
    const int createError = errno;
    ::close(snapshotFd);
    QVERIFY2(created, strerror(createError));

    uid_t uid = 0;
    mode_t mode = 0;
    QVERIFY(lstatDirectoryOwnerAndMode(rootDir.path() + QStringLiteral("/home/alice"), &uid, &mode));
    QCOMPARE(uid, uid_t(1000));
    QCOMPARE(mode, mode_t(0750));
}

/**
 * @brief (b) 非信頼チェーンでは欠けた親をprincipalのfilesystem identityで作成する
 *
 * root/alice (principal 1000) の下に欠けたalice/a/bを作成し
 * 作成物がすべてprincipal所有となり、snapshot側のmodeが適用されることを確認する
 * setgid付きの親でも最終的なfchmodがsetgidを上書きすることを確認する
 */
void TestFilesystemHelpers::beneathRootUntrustedChainCreatesParentsAsPrincipal()
{
    // principalのfilesystem identityでの作成はeuid 0のときだけ働くため、root以外では実行しない
    if (::geteuid() != 0) {
        QSKIP("requires root to exercise the untrusted chain policy");
    }

    const auto runCase = [](mode_t aliceMode) {
        QTemporaryDir rootDir;
        QTemporaryDir snapshotDir;
        QVERIFY(rootDir.isValid() && snapshotDir.isValid());

        const QString alicePath = rootDir.path() + QStringLiteral("/alice");
        QVERIFY(QDir().mkpath(alicePath));
        QCOMPARE(::chown(QFile::encodeName(alicePath).constData(), 1000, 1000), 0);
        QCOMPARE(::chmod(QFile::encodeName(alicePath).constData(), aliceMode), 0);

        QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("alice/a/b")));
        const QString sourceA = snapshotDir.path() + QStringLiteral("/alice/a");
        const QString sourceB = sourceA + QStringLiteral("/b");
        QCOMPARE(::chmod(QFile::encodeName(sourceA).constData(), 0750), 0);
        QCOMPARE(::chmod(QFile::encodeName(sourceB).constData(), 0700), 0);
        QCOMPARE(::chown(QFile::encodeName(sourceA).constData(), 1000, 1000), 0);
        QCOMPARE(::chown(QFile::encodeName(sourceB).constData(), 1000, 1000), 0);

        const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(snapshotFd >= 0);

        errno = 0;
        const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
                rootDir.path(), alicePath + QStringLiteral("/a/b"), snapshotFd, true);
        const int createError = errno;
        ::close(snapshotFd);
        QVERIFY2(created, strerror(createError));

        uid_t uid = 0;
        mode_t mode = 0;
        QVERIFY(lstatDirectoryOwnerAndMode(alicePath + QStringLiteral("/a"), &uid, &mode));
        QCOMPARE(uid, uid_t(1000));
        QCOMPARE(mode, mode_t(0750));
        QVERIFY(lstatDirectoryOwnerAndMode(alicePath + QStringLiteral("/a/b"), &uid, &mode));
        QCOMPARE(uid, uid_t(1000));
        QCOMPARE(mode, mode_t(0700));
    };

    runCase(0755);
    runCase(02755);
}

/**
 * @brief (c) 非信頼チェーンで復元元の所有者がprincipalと異なる場合は作成しない
 *
 * principal 1000のaliceの下に、uid 2000所有のaを復元しようとすると
 * rootはprincipalの権限を越えてaを作れないためEPERMとし、aliceを変更しない
 */
void TestFilesystemHelpers::beneathRootUntrustedChainRejectsForeignSourceOwner()
{
    // 所有者不一致の拒否はeuid 0のときだけ働くため、root以外では実行しない
    if (::geteuid() != 0) {
        QSKIP("requires root to exercise the untrusted chain policy");
    }

    QTemporaryDir rootDir;
    QTemporaryDir snapshotDir;
    QVERIFY(rootDir.isValid() && snapshotDir.isValid());

    const QString alicePath = rootDir.path() + QStringLiteral("/alice");
    QVERIFY(QDir().mkpath(alicePath));
    QCOMPARE(::chown(QFile::encodeName(alicePath).constData(), 1000, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(alicePath).constData(), 0755), 0);

    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("alice/a")));
    const QString sourceA = snapshotDir.path() + QStringLiteral("/alice/a");
    QCOMPARE(::chmod(QFile::encodeName(sourceA).constData(), 0750), 0);
    QCOMPARE(::chown(QFile::encodeName(sourceA).constData(), 2000, 2000), 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    errno = 0;
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            rootDir.path(), alicePath + QStringLiteral("/a"), snapshotFd, true);
    const int createError = errno;
    ::close(snapshotFd);
    QVERIFY(!created);
    QCOMPARE(createError, EPERM);

    // 親の所有者とmodeを変えず、principalの権限外の作成物も残していないことを確認する
    uid_t uid = 0;
    mode_t mode = 0;
    QVERIFY(lstatDirectoryOwnerAndMode(alicePath, &uid, &mode));
    QCOMPARE(uid, uid_t(1000));
    QCOMPARE(mode, mode_t(0755));
    struct stat st {};
    QCOMPARE(::lstat(QFile::encodeName(alicePath + QStringLiteral("/a")).constData(), &st), -1);
    QCOMPARE(errno, ENOENT);
}

/**
 * @brief (d) 非信頼点より下にroot所有の成分が混在する場合は作成しない
 *
 * alice (uid 1000) の下のaがroot所有のまま残っている場合
 * その先のbをprincipalの所有物として作成すると所有者の境界が混ざるためEPERMとし、aを変更しない
 */
void TestFilesystemHelpers::beneathRootUntrustedChainRejectsMixedOwnership()
{
    // 所有者の混在拒否はeuid 0のときだけ働くため、root以外では実行しない
    if (::geteuid() != 0) {
        QSKIP("requires root to exercise the untrusted chain policy");
    }

    QTemporaryDir rootDir;
    QTemporaryDir snapshotDir;
    QVERIFY(rootDir.isValid() && snapshotDir.isValid());

    const QString alicePath = rootDir.path() + QStringLiteral("/alice");
    QVERIFY(QDir().mkpath(alicePath + QStringLiteral("/a")));
    QCOMPARE(::chown(QFile::encodeName(alicePath).constData(), 1000, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(alicePath).constData(), 0755), 0);
    QCOMPARE(::chmod(QFile::encodeName(alicePath + QStringLiteral("/a")).constData(), 0755), 0);

    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("alice/a/b")));
    const QString sourceB = snapshotDir.path() + QStringLiteral("/alice/a/b");
    QCOMPARE(::chmod(QFile::encodeName(sourceB).constData(), 0750), 0);
    QCOMPARE(::chown(QFile::encodeName(sourceB).constData(), 1000, 1000), 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);

    errno = 0;
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(
            rootDir.path(), alicePath + QStringLiteral("/a/b"), snapshotFd, true);
    const int createError = errno;
    ::close(snapshotFd);
    QVERIFY(!created);
    QCOMPARE(createError, EPERM);

    // root所有のaをprincipal所有へ変えず、bも作っていないことを確認する
    uid_t uid = 0;
    mode_t mode = 0;
    QVERIFY(lstatDirectoryOwnerAndMode(alicePath + QStringLiteral("/a"), &uid, &mode));
    QCOMPARE(uid, uid_t(0));
    QCOMPARE(mode, mode_t(0755));
    struct stat st {};
    QCOMPARE(::lstat(QFile::encodeName(alicePath + QStringLiteral("/a/b")).constData(), &st), -1);
    QCOMPARE(errno, ENOENT);
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
    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
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
        const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), victimPath, 0644);
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
        const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), okPath, 0644);
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
    QVERIFY(safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), QString(), 0644) < 0);
    QCOMPARE(errno, EINVAL);
    QVERIFY(safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), QStringLiteral("/"), 0644) < 0);
    QCOMPARE(errno, EINVAL);
    QVERIFY(safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), rootDir.path(), 0644) < 0);
    QCOMPARE(errno, EINVAL);

    // 連続スラッシュは正規化されてroot配下に収まる (write系は親を作らないため先に作成)
    const QString messyDir = rootDir.path() + QStringLiteral("//messy///dir");
    QVERIFY(safeCreateDirectoryBeneathRoot(rootDir.path(), messyDir, 0755));
    QVERIFY(QDir(rootDir.path() + QStringLiteral("/messy/dir")).exists());

    const QString messyPath = messyDir + QStringLiteral("/file.txt");
    const int messyFd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), messyPath, 0644);
    QVERIFY(messyFd >= 0);
    ::close(messyFd);
    QVERIFY(QFile::exists(rootDir.path() + QStringLiteral("/messy/dir/file.txt")));

    // 非常に深いネスト: kernel上限に当たっても安全に失敗、またはroot配下で成功する
    QString deepRelative;
    for (int i = 0; i < 400; ++i) {
        deepRelative += QStringLiteral("d%1/").arg(i % 10);
    }
    deepRelative += QStringLiteral("leaf.txt");
    const int deepFd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), deepRelative, 0644);
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
    const int nonUtf8Fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), nonUtf8Destination,
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
    QVERIFY(safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), nulDestination, 0644) < 0);
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
    QVERIFY(safeOpenRegularFileWriteBeneathRootForTesting(swappedRoot,
                                                swappedRoot + QStringLiteral("/f.txt"),
                                                0644) < 0);
    QVERIFY(isAttackRejectionErrno(errno));
    QVERIFY(QDir(evilRootDir).entryList(QDir::AllEntries | QDir::Hidden
                                        | QDir::NoDotAndDotDot).isEmpty());
}

/**
 * @brief 深すぎるディレクトリの再帰削除が、fdを使い切る前に何も削除せず止まることを検証する
 *
 * 一般ユーザが作れる深い木を削除させて、rootサービスのfdを枯渇させたりスタックを溢れさせたりできないようにする
 */
void TestFilesystemHelpers::beneathRootRemovalStopsAtDepthLimit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();

    // 上限 (256段) を超える深さの木を、dirfd相対で作る (パス長の上限に掛からないようにするため)
    const QString deepTop = root + QStringLiteral("/deep");
    QVERIFY(QDir().mkpath(deepTop));
    int currentFd = ::open(QFile::encodeName(deepTop).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(currentFd >= 0);
    for (int depth = 0; depth < 300; ++depth) {
        QVERIFY(::mkdirat(currentFd, "d", 0700) == 0);
        const int nextFd = ::openat(currentFd, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ::close(currentFd);
        QVERIFY(nextFd >= 0);
        currentFd = nextFd;
    }
    QVERIFY(::mkdirat(currentFd, "leaf", 0700) == 0);
    ::close(currentFd);

    errno = 0;
    QVERIFY(!safeRemoveAllBeneathRoot(root, deepTop));
    QCOMPARE(errno, ELOOP);
    QVERIFY(QFileInfo::exists(deepTop + QStringLiteral("/d/d/d")));

    // 上限内の深さであれば、ファイルを含む木を全て削除できる
    const QString shallowTop = root + QStringLiteral("/shallow");
    QString nested = shallowTop;
    for (int depth = 0; depth < 20; ++depth) {
        nested += QStringLiteral("/n");
    }
    QVERIFY(QDir().mkpath(nested));
    QFile nestedFile(nested + QStringLiteral("/file"));
    QVERIFY(nestedFile.open(QIODevice::WriteOnly));
    nestedFile.close();
    QFile topFile(shallowTop + QStringLiteral("/top-file"));
    QVERIFY(topFile.open(QIODevice::WriteOnly));
    topFile.close();
    QVERIFY(safeRemoveAllBeneathRoot(root, shallowTop));
    QVERIFY(!QFileInfo::exists(shallowTop));

    // QTemporaryDirの後片付けがパス長の上限で失敗しないよう、深い木は下から順に消す
    int fds[302];
    fds[0] = ::open(QFile::encodeName(deepTop).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(fds[0] >= 0);
    for (int depth = 1; depth <= 300; ++depth) {
        fds[depth] = ::openat(fds[depth - 1], "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(fds[depth] >= 0);
    }
    QVERIFY(::unlinkat(fds[300], "leaf", AT_REMOVEDIR) == 0);
    for (int depth = 300; depth >= 1; --depth) {
        ::close(fds[depth]);
        QVERIFY(::unlinkat(fds[depth - 1], "d", AT_REMOVEDIR) == 0);
    }
    ::close(fds[0]);
}

/**
 * @brief 復元先の名前解決が、途中のmountポイントを越えないことを検証する
 *
 * libsnapperの差分は別のファイルシステムへ降りないため、mountを越えるエントリは偽物として扱う
 * [/]と別のファイルシステムである/procを使い、同じファイルシステムの環境では検証を省略する
 */
void TestFilesystemHelpers::beneathRootResolutionRefusesMountBoundaries()
{
    struct stat rootInfo;
    struct stat procInfo;
    QCOMPARE(::stat("/", &rootInfo), 0);
    if (::stat("/proc/sys", &procInfo) != 0 || procInfo.st_dev == rootInfo.st_dev) {
        QSKIP("/proc is not a separate mount in this environment");
    }

    QByteArray leafName;
    errno = 0;
    const int parentFd = openDestinationParentBeneathRoot(QStringLiteral("/"),
                                                          QStringLiteral("/proc/sys/kernel"), &leafName);
    if (parentFd >= 0) {
        ::close(parentFd);
    }
    QVERIFY(parentFd < 0);
    QCOMPARE(errno, EXDEV);

    errno = 0;
    const int directoryFd = safeOpenDirectoryBeneathRoot(QStringLiteral("/"), QStringLiteral("proc/sys"),
                                                         false, 0);
    if (directoryFd >= 0) {
        ::close(directoryFd);
    }
    QVERIFY(directoryFd < 0);
    QCOMPARE(errno, EXDEV);

    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const int sourceFd = ::open(QFile::encodeName(sourceDir.path()).constData(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(sourceFd >= 0);
    errno = 0;
    const bool created = safeCreateParentDirectoriesFromSourceBeneathRoot(QStringLiteral("/"),
                                                                          QStringLiteral("/proc/sys"),
                                                                          sourceFd, false);
    const int createError = errno;
    ::close(sourceFd);
    QVERIFY(!created);
    QCOMPARE(createError, EXDEV);
}

/** @brief 正しい木と差し替え・所有者・種別の異常を対比する行を提供する */
void TestFilesystemHelpers::snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths_data()
{
    QTest::addColumn<QString>("variant");
    QTest::addColumn<int>("expectedErrno");
    QTest::newRow("a-valid-tree") << QStringLiteral("valid") << 0;
    QTest::newRow("b-snapshots-symlink") << QStringLiteral("snapshots-symlink") << ELOOP;
    QTest::newRow("c-snapshot-symlink") << QStringLiteral("snapshot-symlink") << ELOOP;
    QTest::newRow("d-snapshots-user-owned") << QStringLiteral("snapshots-user-owned") << EPERM;
    QTest::newRow("e-number-world-writable") << QStringLiteral("number-world-writable") << EPERM;
    QTest::newRow("f-user-owned-subvolume") << QStringLiteral("user-owned-subvolume") << 0;
    QTest::newRow("g-snapshot-regular-file") << QStringLiteral("snapshot-regular-file") << ENOTDIR;
}

/** @brief symlinkを辿らず、rootが管理する .snapshots と番号ディレクトリの下だけを開く */
void TestFilesystemHelpers::snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths()
{
    // 所有者の判定はroot所有の木を前提にするため、rootでのみ検証する
    if (::geteuid() != 0) {
        QSKIP("Requires root to build root-owned .snapshots trees");
    }
    QFETCH(QString, variant);
    QFETCH(int, expectedErrno);

    QTemporaryDir subvolume;
    QVERIFY(subvolume.isValid());
    const QString base = subvolume.path();
    QCOMPARE(::chmod(QFile::encodeName(base).constData(), 0755), 0);

    // symlinkの差し替え先にも正しい木を置き、辿った場合は開けてしまう状態で拒否されることを確かめる
    const QString realRoot = base + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realRoot + QStringLiteral("/.snapshots/5/snapshot")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/realsnapshot")));
    if (variant == QStringLiteral("snapshots-symlink")) {
        QCOMPARE(::symlink("real/.snapshots", QFile::encodeName(base + QStringLiteral("/.snapshots")).constData()), 0);
    }
    else {
        QVERIFY(QDir().mkpath(base + QStringLiteral("/.snapshots/5")));
        const QString snapshotPath = base + QStringLiteral("/.snapshots/5/snapshot");
        if (variant == QStringLiteral("snapshot-symlink")) {
            QCOMPARE(::symlink("../../realsnapshot", QFile::encodeName(snapshotPath).constData()), 0);
        }
        else if (variant == QStringLiteral("snapshot-regular-file")) {
            QFile file(snapshotPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        else {
            QVERIFY(QDir().mkpath(snapshotPath));
        }
    }
    if (variant == QStringLiteral("snapshots-user-owned")) {
        QCOMPARE(::chown(QFile::encodeName(base + QStringLiteral("/.snapshots")).constData(), 1000, 1000), 0);
    }
    if (variant == QStringLiteral("number-world-writable")) {
        QCOMPARE(::chmod(QFile::encodeName(base + QStringLiteral("/.snapshots/5")).constData(), 0777), 0);
    }
    if (variant == QStringLiteral("user-owned-subvolume")) {
        QCOMPARE(::chown(QFile::encodeName(base).constData(), 1000, 1000), 0);
    }

    const int subvolumeFd = ::open(QFile::encodeName(base).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(subvolumeFd >= 0);
    struct stat opened {};
    errno = 0;
    const int snapshotFd = openSnapshotDirectoryBeneathSubvolume(subvolumeFd, 5, &opened);
    const int openError = errno;
    ::close(subvolumeFd);
    if (snapshotFd >= 0) {
        ::close(snapshotFd);
    }

    if (expectedErrno == 0) {
        QVERIFY(snapshotFd >= 0);
        struct stat expected {};
        QCOMPARE(::lstat(QFile::encodeName(base + QStringLiteral("/.snapshots/5/snapshot")).constData(), &expected), 0);
        QCOMPARE(opened.st_dev, expected.st_dev);
        QCOMPARE(opened.st_ino, expected.st_ino);
        return;
    }
    QVERIFY(snapshotFd < 0);
    // 末尾がO_DIRECTORY | O_NOFOLLOWのsymlinkはENOTDIRで報告されることがあり、どちらも辿らない拒否である
    if (expectedErrno == ELOOP) {
        QVERIFY2(openError == ELOOP || openError == ENOTDIR, qPrintable(QString::number(openError)));
    }
    else {
        QCOMPARE(openError, expectedErrno);
    }
}
