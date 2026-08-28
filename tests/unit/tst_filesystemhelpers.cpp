#include <QtTest/QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    void safeOpenRegularFileWriteCreatesRegularFile();
    void safeOpenRegularFileWriteTruncatesExistingRegularFile();
    void safeOpenRegularFileWriteAppliesMode();
    void safeOpenRegularFileWriteFailsWhenParentMissing();
    void safeOpenRegularFileWriteRejectsSymlink();
    void safeOpenRegularFileReadRejectsSymlink();
    void safeOpenDirectoryRejectsSymlinkPath();
    void safeRenamePathNoFollowRenamesWithinTrustedParent();
    void safeSymlinkHelpersRespectTrustedParents();
    void safeReadLinkNoFollowHandlesLargeTarget();
    void safeLstatReportsSymlinkItself();
    void safeRemoveAllRemovesSingleFile();
    void safeRemoveAllRemovesTreeWithoutFollowingSymlink();
    void safeRemoveAllRejectsSymlinkIntermediateComponent();

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

void TestFilesystemHelpers::safeOpenRegularFileWriteCreatesRegularFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/state.txt");
    const int fd = safeOpenRegularFileWrite(filePath, 0644);
    QVERIFY(fd >= 0);

    const QByteArray payload("12345");
    QCOMPARE(::write(fd, payload.constData(), static_cast<size_t>(payload.size())), payload.size());
    ::close(fd);

    QFile file(filePath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), payload);
}

void TestFilesystemHelpers::safeOpenRegularFileWriteTruncatesExistingRegularFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/state.txt");
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("old content that should be truncated");
    }

    const int fd = safeOpenRegularFileWrite(filePath, 0644);
    QVERIFY(fd >= 0);

    const QByteArray payload("new");
    QCOMPARE(::write(fd, payload.constData(), static_cast<size_t>(payload.size())), payload.size());
    ::close(fd);

    QFile file(filePath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), payload);
}

void TestFilesystemHelpers::safeOpenRegularFileWriteAppliesMode()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/mode-0600.txt");
    const int fd = safeOpenRegularFileWrite(filePath, 0600);
    QVERIFY(fd >= 0);
    ::close(fd);

    struct stat st;
    QVERIFY(::lstat(filePath.toUtf8().constData(), &st) == 0);
    QCOMPARE(st.st_mode & 0777, 0600);
}

void TestFilesystemHelpers::safeOpenRegularFileWriteFailsWhenParentMissing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/missing-parent/file.txt");
    const int fd = safeOpenRegularFileWrite(filePath, 0644);
    QVERIFY(fd < 0);
}

void TestFilesystemHelpers::safeOpenRegularFileWriteRejectsSymlink()
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

    const int fd = safeOpenRegularFileWrite(symlinkPath, 0644);
    QVERIFY(fd < 0);

    QFile verify(realFile);
    QVERIFY(verify.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(verify.readAll(), QByteArray("safe"));
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

void TestFilesystemHelpers::safeRenamePathNoFollowRenamesWithinTrustedParent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.path() + QStringLiteral("/before.txt");
    {
        QFile file(sourcePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("payload");
    }

    const QString destPath = tempDir.path() + QStringLiteral("/after.txt");
    QVERIFY(safeRenamePathNoFollow(sourcePath, destPath));
    QVERIFY(!QFile::exists(sourcePath));
    QVERIFY(QFile::exists(destPath));

    const QString realDir = tempDir.path() + QStringLiteral("/real");
    QVERIFY(QDir().mkpath(realDir));
    const QString parentSymlink = tempDir.path() + QStringLiteral("/link-parent");
    QVERIFY(::symlink(realDir.toUtf8().constData(), parentSymlink.toUtf8().constData()) == 0);

    const QString badSource = parentSymlink + QStringLiteral("/from.txt");
    const QString badDest = parentSymlink + QStringLiteral("/to.txt");
    QVERIFY(!safeRenamePathNoFollow(badSource, badDest));
}

void TestFilesystemHelpers::safeSymlinkHelpersRespectTrustedParents()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray target("target-value");
    const QString linkPath = tempDir.path() + QStringLiteral("/ok-link");
    QVERIFY(safeCreateSymlinkNoFollow(target, linkPath));

    QByteArray observedTarget;
    QVERIFY(safeReadLinkNoFollow(linkPath, &observedTarget));
    QCOMPARE(observedTarget, target);

    struct stat st;
    QVERIFY(::lstat(linkPath.toUtf8().constData(), &st) == 0);
    struct timespec ts[2];
    ts[0] = st.st_atim;
    ts[1] = st.st_mtim;
    bool ownerUpdated = false;
    bool timesUpdated = false;
    QVERIFY(safeSetSymlinkMetadataNoFollow(linkPath, ::getuid(), ::getgid(), ts,
                                           &ownerUpdated, &timesUpdated));
    QVERIFY(ownerUpdated);
    QVERIFY(timesUpdated);

    const QString realDir = tempDir.path() + QStringLiteral("/real-links");
    QVERIFY(QDir().mkpath(realDir));
    const QString parentSymlink = tempDir.path() + QStringLiteral("/symlink-parent");
    QVERIFY(::symlink(realDir.toUtf8().constData(), parentSymlink.toUtf8().constData()) == 0);
    const QString escapedPath = parentSymlink + QStringLiteral("/child-link");

    QVERIFY(!safeCreateSymlinkNoFollow(target, escapedPath));
    QVERIFY(!safeReadLinkNoFollow(escapedPath, &observedTarget));
    QVERIFY(!safeSetSymlinkMetadataNoFollow(escapedPath, ::getuid(), ::getgid(), ts,
                                            &ownerUpdated, &timesUpdated));
}

void TestFilesystemHelpers::safeReadLinkNoFollowHandlesLargeTarget()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray largeTarget(2048, 'a');
    const QString linkPath = tempDir.path() + QStringLiteral("/large-link");
    QVERIFY(safeCreateSymlinkNoFollow(largeTarget, linkPath));

    QByteArray observedTarget;
    QVERIFY(safeReadLinkNoFollow(linkPath, &observedTarget));
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

    QVERIFY(safeRemoveAll(treePath));
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

    QVERIFY(safeRemoveAll(filePath));
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
    QVERIFY(!safeRemoveAll(victimPath));
    QVERIFY(QFile::exists(realFile));
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
    QVERIFY(safeRemoveAll(componentPath));
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
    QVERIFY(safeRemoveAll(victimPath));
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
    QVERIFY(safeRemoveAll(rootDir.path() + QStringLiteral("/a")));
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

void TestFilesystemHelpers::beneathRootSymlinkHelpersApplyWithinRoot()
{
    QTemporaryDir rootDir;
    QVERIFY(rootDir.isValid());

    QVERIFY(QDir().mkpath(rootDir.path() + QStringLiteral("/sub")));

    const QByteArray target("target-value");
    const QString linkPath = rootDir.path() + QStringLiteral("/sub/link");
    QVERIFY(safeCreateSymlinkNoFollowBeneathRoot(rootDir.path(), target, linkPath));

    QByteArray observedTarget;
    QVERIFY(safeReadLinkNoFollow(linkPath, &observedTarget));
    QCOMPARE(observedTarget, target);

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

QTEST_APPLESS_MAIN(TestFilesystemHelpers)
#include "tst_filesystemhelpers.moc"
