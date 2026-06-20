#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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

QTEST_APPLESS_MAIN(TestFilesystemHelpers)
#include "tst_filesystemhelpers.moc"
