/**
 * @file tst_configpathvalidation.cpp
 * @brief src/dbusservice/configpathvalidation.hの単体テスト
 *
 * 設定名とsubvolumeパスの検証は、文字列だけで判定できるためデータ駆動テストで境界を確認する
 * ディレクトリとファイルの信頼判定は、root所有でgroup/other書き込み不可という前提を実際のパスで確かめる
 * root権限を必要とする検証は行わず、判定に使える既存パスが無い場合はQSKIPで明示する
 */

#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "configpathvalidation.h"

using namespace qsnapper::security;

namespace {

/**
 * @brief 指定したパスがroot所有でgroup/otherから書き換えられない実在パスかを調べる
 * @param path 判定対象のパス
 * @return 前提を満たす場合だけtrue
 *
 * symlinkを辿らずにlstatで判定するため、root所有symlinkも対象にできる
 */
bool isTrustedRootPath(const char *path)
{
    struct stat st {};
    if (lstat(path, &st) != 0) {
        return false;
    }
    return st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

/**
 * @brief 指定したパスがroot所有の通常ファイルかを調べる
 * @param path 判定対象のパス
 * @return 通常ファイルでroot所有の場合だけtrue
 */
bool isTrustedRootRegularFile(const char *path)
{
    struct stat st {};
    if (lstat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) && isTrustedRootPath(path);
}

/**
 * @brief ファイルシステム種別とinode番号を取得する
 * @param path 判定対象のパス
 * @param type ファイルシステム種別を受け取る
 * @param ino inode番号を受け取る
 * @return 取得できた場合だけtrue
 */
bool statfsOf(const char *path, long *type, ino_t *ino)
{
    struct statfs fs {};
    struct stat st {};
    if (statfs(path, &fs) != 0 || stat(path, &st) != 0) {
        return false;
    }
    *type = static_cast<long>(fs.f_type);
    *ino = st.st_ino;
    return true;
}

} // namespace

/**
 * @class TestConfigPathValidation
 * @brief 設定名、subvolumeパス、信頼できるディレクトリとファイルの判定を検証する
 */
class TestConfigPathValidation : public QObject
{
    Q_OBJECT

private slots:
    void testNewConfigName_data();
    void testNewConfigName();
    void testConfigSubvolumePath_data();
    void testConfigSubvolumePath();
    void openTrustedDirectoryAcceptsRootOwnedDirectories();
    void openTrustedDirectoryRejectsUntrustedTemporaryDirectory();
    void openTrustedDirectoryRejectsSymlink();
    void openTrustedDirectoryReportsMissing();
    void openTrustedDirectoryRejectsRegularFile();
    void checkTrustedRegularFileAcceptsRootOwnedFile();
    void checkTrustedRegularFileRejectsDirectory();
    void checkTrustedRegularFileReportsMissing();
    void checkTrustedAbsentAcceptsAbsentPath();
    void checkTrustedAbsentRejectsExistingPath();
    void isBtrfsSubvolumeRootRejectsNonBtrfsDirectory();
    void isBtrfsSubvolumeRootAcceptsRootOnBtrfs();
};

/**
 * @brief 設定名の受理と拒否を境界と危険な文字で確認する
 */
void TestConfigPathValidation::testNewConfigName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("expected");

    QTest::newRow("single alnum") << QStringLiteral("a") << true;
    QTest::newRow("upper and digit") << QStringLiteral("A0") << true;
    QTest::newRow("underscore hyphen") << QStringLiteral("a_b-c") << true;
    // 設定名はsnapperの設定ファイル名に使われるため拡張子風のdotも拒否する
    QTest::newRow("dot inside") << QStringLiteral("a.b") << false;
    QTest::newRow("max length 64") << QStringLiteral("a").repeated(64) << true;
    // 65文字はsnapperd側の上限を超えるため受け付けない
    QTest::newRow("length 65") << QStringLiteral("a").repeated(65) << false;
    QTest::newRow("empty") << QString() << false;
    // 先頭のhyphenはコマンドライン引数と誤認され得るため拒否する
    QTest::newRow("leading hyphen") << QStringLiteral("-a") << false;
    QTest::newRow("leading underscore") << QStringLiteral("_a") << false;
    QTest::newRow("dot") << QStringLiteral(".") << false;
    QTest::newRow("dot dot") << QStringLiteral("..") << false;
    QTest::newRow("slash") << QStringLiteral("a/b") << false;
    QTest::newRow("space") << QStringLiteral("a b") << false;
    QTest::newRow("newline") << QStringLiteral("a\n") << false;
    QTest::newRow("double quote") << QStringLiteral("a\"") << false;
    QTest::newRow("single quote") << QStringLiteral("a'") << false;
    QTest::newRow("dollar") << QStringLiteral("a$") << false;
    QTest::newRow("non ascii") << QStringLiteral("a\xC3\xA9") << false;
}

/**
 * @brief 設定名の検証結果が期待と一致することを確認する
 */
void TestConfigPathValidation::testNewConfigName()
{
    QFETCH(QString, name);
    QFETCH(bool, expected);

    QCOMPARE(validateNewConfigName(name), expected);
}

/**
 * @brief subvolumeパスの受理と拒否を境界と危険な要素で確認する
 */
void TestConfigPathValidation::testConfigSubvolumePath_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("expected");

    QTest::newRow("root") << QStringLiteral("/") << true;
    QTest::newRow("single component") << QStringLiteral("/home") << true;
    QTest::newRow("allowed symbols") << QStringLiteral("/var/lib/x@y+z") << true;
    QTest::newRow("nested") << QStringLiteral("/var/lib/qsnapper/test") << true;
    // 相対パスはsnapperdが基準ディレクトリを変えて解釈し得るため拒否する
    QTest::newRow("relative") << QStringLiteral("home") << false;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("double slash") << QStringLiteral("//x") << false;
    QTest::newRow("trailing slash") << QStringLiteral("/x/") << false;
    QTest::newRow("dot component") << QStringLiteral("/x/./y") << false;
    QTest::newRow("dot dot component") << QStringLiteral("/x/../y") << false;
    QTest::newRow("space") << QStringLiteral("/x y") << false;
    QTest::newRow("double quote") << QStringLiteral("/x\"") << false;
    QTest::newRow("single quote") << QStringLiteral("/x'") << false;
    QTest::newRow("dollar") << QStringLiteral("/x$y") << false;
    QTest::newRow("backslash") << QStringLiteral("/x\\y") << false;
    QTest::newRow("control char") << QStringLiteral("/x\x01y") << false;
    // PATH_MAXを超える長さはopenatで切り詰められる可能性があるため拒否する
    QTest::newRow("4096 bytes") << QStringLiteral("/") + QStringLiteral("a").repeated(4095) << false;
    // 1コンポーネントの上限を超えるとファイルシステムごとに扱いが変わるため拒否する
    QTest::newRow("component 256") << QStringLiteral("/") + QStringLiteral("a").repeated(256) << false;
}

/**
 * @brief subvolumeパスの検証結果が期待と一致することを確認する
 */
void TestConfigPathValidation::testConfigSubvolumePath()
{
    QFETCH(QString, path);
    QFETCH(bool, expected);

    QCOMPARE(validateConfigSubvolumePath(path), expected);
}

/**
 * @brief root所有のディレクトリだけが信頼対象として開けることを確認する
 */
void TestConfigPathValidation::openTrustedDirectoryAcceptsRootOwnedDirectories()
{
    const QStringList candidates {QStringLiteral("/usr"), QStringLiteral("/")};
    for (const QString &path : candidates) {
        if (!isTrustedRootPath(path.toLocal8Bit().constData())) {
            QSKIP("root所有のディレクトリが無い環境のため判定できない");
        }

        int fd = -1;
        struct stat st {};
        QCOMPARE(openTrustedDirectory(path, &fd, &st), TrustedPathResult::Ok);
        QVERIFY(fd >= 0);
        QVERIFY(S_ISDIR(st.st_mode));
        ::close(fd);
    }
}

/**
 * @brief 誰でも書き込める親を持つディレクトリは信頼対象にならないことを確認する
 */
void TestConfigPathValidation::openTrustedDirectoryRejectsUntrustedTemporaryDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // /tmpはsticky bit付きで誰でも書き込めるため、配下のパスは差し替えを防げない
    int fd = -1;
    QCOMPARE(openTrustedDirectory(dir.path(), &fd), TrustedPathResult::Untrusted);
    QCOMPARE(fd, -1);
}

/**
 * @brief 途中にsymlinkがあるパスを辿らずに拒否することを確認する
 */
void TestConfigPathValidation::openTrustedDirectoryRejectsSymlink()
{
    // usrmerge環境では/binや/lib64がroot所有のsymlinkになっている
    const QStringList candidates {QStringLiteral("/bin"), QStringLiteral("/lib64"),
                                  QStringLiteral("/sbin")};
    for (const QString &path : candidates) {
        struct stat st {};
        if (lstat(path.toLocal8Bit().constData(), &st) != 0 || !S_ISLNK(st.st_mode)) {
            continue;
        }
        if (!isTrustedRootPath("/")) {
            QSKIP("root所有のルートディレクトリが無い環境のため判定できない");
        }

        int fd = -1;
        QCOMPARE(openTrustedDirectory(path, &fd), TrustedPathResult::SymbolicLink);
        QCOMPARE(fd, -1);
        return;
    }
    QSKIP("root所有のsymlinkが無い環境のため判定できない");
}

/**
 * @brief 存在しないパスをMissingとして区別することを確認する
 */
void TestConfigPathValidation::openTrustedDirectoryReportsMissing()
{
    int fd = -1;
    QCOMPARE(openTrustedDirectory(QStringLiteral("/nonexistent-qsnapper-xyz"), &fd),
             TrustedPathResult::Missing);
    QCOMPARE(fd, -1);
}

/**
 * @brief 通常ファイルをディレクトリとして開かないことを確認する
 */
void TestConfigPathValidation::openTrustedDirectoryRejectsRegularFile()
{
    const char *candidate = "/etc/hostname";
    if (!isTrustedRootRegularFile(candidate)) {
        QSKIP("root所有の通常ファイルが無い環境のため判定できない");
    }

    int fd = -1;
    QCOMPARE(openTrustedDirectory(QString::fromLatin1(candidate), &fd), TrustedPathResult::WrongType);
    QCOMPARE(fd, -1);
}

/**
 * @brief root所有で単一リンクの通常ファイルを信頼できると判定することを確認する
 */
void TestConfigPathValidation::checkTrustedRegularFileAcceptsRootOwnedFile()
{
    // /etc/os-releaseはsymlinkの環境があるため、通常ファイルのときだけ検証する
    const QStringList candidates {QStringLiteral("/etc/os-release"),
                                  QStringLiteral("/etc/hostname")};
    for (const QString &path : candidates) {
        if (!isTrustedRootRegularFile(path.toLocal8Bit().constData())) {
            continue;
        }
        struct stat st {};
        if (lstat(path.toLocal8Bit().constData(), &st) != 0) {
            continue;
        }
        if (st.st_nlink != 1) {
            continue;
        }

        QCOMPARE(checkTrustedRegularFile(path), TrustedPathResult::Ok);
        return;
    }
    QSKIP("root所有で単一リンクの通常ファイルが無い環境のため判定できない");
}

/**
 * @brief ディレクトリを通常ファイルとして受け付けないことを確認する
 */
void TestConfigPathValidation::checkTrustedRegularFileRejectsDirectory()
{
    QCOMPARE(checkTrustedRegularFile(QStringLiteral("/usr")), TrustedPathResult::WrongType);
}

/**
 * @brief 存在しないパスを通常ファイルの検証でMissingとして扱うことを確認する
 */
void TestConfigPathValidation::checkTrustedRegularFileReportsMissing()
{
    QCOMPARE(checkTrustedRegularFile(QStringLiteral("/etc/qsnapper-nonexistent-xyz")),
             TrustedPathResult::Missing);
}

/**
 * @brief 存在しないパスを、親が信頼できる場合だけ受け入れることを確認する
 */
void TestConfigPathValidation::checkTrustedAbsentAcceptsAbsentPath()
{
    QCOMPARE(checkTrustedAbsent(QStringLiteral("/etc/qsnapper-nonexistent-xyz")), TrustedPathResult::Ok);
}

/**
 * @brief 既存のディレクトリを不在として扱わないことを確認する
 */
void TestConfigPathValidation::checkTrustedAbsentRejectsExistingPath()
{
    QCOMPARE(checkTrustedAbsent(QStringLiteral("/etc")), TrustedPathResult::WrongType);
}

/**
 * @brief Btrfs以外のディレクトリをsubvolumeのrootと判定しないことを確認する
 */
void TestConfigPathValidation::isBtrfsSubvolumeRootRejectsNonBtrfsDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    long type = 0;
    ino_t ino = 0;
    if (!statfsOf(dir.path().toLocal8Bit().constData(), &type, &ino)) {
        QSKIP("ファイルシステム種別を取得できないため判定できない");
    }

    const int fd = ::open(dir.path().toLocal8Bit().constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    const bool isSubvolume = isBtrfsSubvolumeRoot(fd);
    ::close(fd);

    QCOMPARE(isSubvolume, false);
}

/**
 * @brief ルートがBtrfsのsubvolume rootである場合だけ受理することを確認する
 */
void TestConfigPathValidation::isBtrfsSubvolumeRootAcceptsRootOnBtrfs()
{
    long type = 0;
    ino_t ino = 0;
    if (!statfsOf("/", &type, &ino) || type != BTRFS_SUPER_MAGIC || ino != 256) {
        QSKIP("ルートがBtrfsのsubvolume rootではないため判定できない");
    }

    const int fd = ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    const bool isSubvolume = isBtrfsSubvolumeRoot(fd);
    ::close(fd);

    QCOMPARE(isSubvolume, true);
}

QTEST_MAIN(TestConfigPathValidation)
#include "tst_configpathvalidation.moc"
