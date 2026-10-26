/**
 * @file tst_filesystemhelpers_beneathroot.cpp
 * @brief beneath-root宛先解決の検証(前半)
 *
 * 宛先検証とディレクトリ解決、書込・mkdir・rename・削除・symlinkの差し替え耐性を検証する
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filesystemhelpers.h"

#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;
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
    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    QWARN(qPrintable(QStringLiteral("write errno=%1 (%2)").arg(err).arg(strerror(err))));
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
    QVERIFY(safeRemoveAllBeneathRoot(rootDir.path(), victimPath));
    QVERIFY(::symlink((outsideDir.path() + QStringLiteral("/secret.txt")).toUtf8().constData(),
                      victimPath.toUtf8().constData()) == 0);

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
    QWARN(qPrintable(QStringLiteral("leaf-symlink write errno=%1 (%2)").arg(err).arg(strerror(err))));
    QVERIFY(isAttackRejectionErrno(err));

    verifyOutsideUntouched(outsideDir, sentinel, fingerprintBefore);
}

/**
 * @brief 中間成分がdangling symlinkの場合も、不在とみなさず拒否することを確認する
 *
 * 不在とみなして新規作成すると、攻撃者が後からlinkの先を作って書き込み先を差し替えられる
 */
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
    QVERIFY(safeRemoveAllBeneathRoot(rootDir.path(), rootDir.path() + QStringLiteral("/a")));
    QVERIFY(::symlink(QStringLiteral("/nonexistent/qsnapper-dangling-target").toUtf8().constData(),
                      (rootDir.path() + QStringLiteral("/a")).toUtf8().constData()) == 0);

    errno = 0;
    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), victimPath, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
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
    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), destination, 0644);
    const int err = errno;
    QVERIFY(fd < 0);
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

    const int fd = safeOpenRegularFileWriteBeneathRootForTesting(rootDir.path(), destination, 0644);
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
