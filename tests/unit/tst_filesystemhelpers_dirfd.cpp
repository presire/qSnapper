/**
 * @file tst_filesystemhelpers_dirfd.cpp
 * @brief dirfd相対解決の検証
 *
 * dirfd相対のstat・読出・不在確認とin-root解決、復元先walkの検証を集める
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "filesystemhelpers.h"
#include "filesystemhelpers_internal.h"

#include "tst_filesystemhelpers.h"

namespace {
/**
 * @class NestedSubvolumeFixture
 * @brief 復元先walkのテストに使うnested btrfs subvolume
 *
 * 環境変数で指定されたbtrfs上のディレクトリにsubvolumeを作る
 * テストが失敗してもsubvolumeを残さないよう、破棄時に内側のfileとsubvolumeを順に消す
 * 作成に失敗した理由はerror()で取得でき、呼び出し側がQFAILする
 */
class NestedSubvolumeFixture
{
public:
    /**
     * @brief テスト用ルート、nested subvolume、subvolume内のfileを作成する
     * @param base btrfs上のベースディレクトリ
     */
    explicit NestedSubvolumeFixture(const QString &base)
        : m_root(base + QStringLiteral("/root-") + QString::number(::getpid()))
        , m_subvolume(m_root + QStringLiteral("/nested"))
        , m_insideFile(m_subvolume + QStringLiteral("/inside"))
    {
        // 前回の異常終了で残った同名ディレクトリがあるとsubvolume作成が失敗するため、先に消す
        if (!QDir(m_root).removeRecursively()) {
            m_error = QStringLiteral("cannot remove stale test root %1").arg(m_root);
            return;
        }
        if (!QDir().mkpath(m_root)) {
            m_error = QStringLiteral("cannot create test root %1").arg(m_root);
            return;
        }

        qsnapper::security::UniqueFd rootFd(::open(m_root.toUtf8().constData(),
                                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!rootFd.isValid()) {
            m_error = QStringLiteral("cannot open test root: %1")
                              .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }

        struct btrfs_ioctl_vol_args args;
        std::memset(&args, 0, sizeof(args));
        std::strncpy(args.name, "nested", sizeof(args.name) - 1);
        const int created = ::ioctl(rootFd.get(), BTRFS_IOC_SUBVOL_CREATE, &args);
        const int createErrno = errno;
        if (created < 0) {
            m_error = QStringLiteral("cannot create nested subvolume: %1")
                              .arg(QString::fromLocal8Bit(std::strerror(createErrno)));
            return;
        }

        qsnapper::security::UniqueFd fileFd(::open(m_insideFile.toUtf8().constData(),
                                                   O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                                   0644));
        if (!fileFd.isValid()) {
            m_error = QStringLiteral("cannot create file inside subvolume: %1")
                              .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }
        m_valid = true;
    }

    /**
     * @brief 作成したfile、subvolume、テスト用ルートを削除する
     */
    ~NestedSubvolumeFixture()
    {
        ::unlink(m_insideFile.toUtf8().constData());
        ::rmdir(m_subvolume.toUtf8().constData());
        ::rmdir(m_root.toUtf8().constData());
    }

    /**
     * @brief コピーを禁止する
     *
     * 2つのデストラクタが同じsubvolumeとfileを二重に削除しないため
     */
    NestedSubvolumeFixture(const NestedSubvolumeFixture &) = delete;

    /**
     * @brief コピー代入を禁止する
     *
     * 所有権が二重になり二重削除を招かないため
     */
    NestedSubvolumeFixture &operator=(const NestedSubvolumeFixture &) = delete;

    /**
     * @brief subvolumeと内側のfileを作成できたかを返す
     * @return 作成に成功した場合true
     */
    bool isValid() const
    {
        return m_valid;
    }

    /**
     * @brief 作成に失敗した理由を返す
     * @return 失敗理由、成功時は空文字列
     */
    QString error() const
    {
        return m_error;
    }

    /**
     * @brief テスト用ルートのパスを返す
     * @return root-<pid>形式の絶対パス
     */
    QString root() const
    {
        return m_root;
    }

private:
    QString m_root;             // テストごとの一時ルート (root-<pid>)
    QString m_subvolume;        // ルート直下に作るnested subvolume
    QString m_insideFile;       // subvolume内に作る通常ファイル
    QString m_error;            // 作成に失敗した理由
    bool m_valid = false;       // subvolumeとfileを作成できたか
};

/**
 * @brief btrfsのテスト基盤が使えるかを確認する
 *
 * 環境変数の設定有無は呼び出し側が判定する
 * 設定済みなのに使えない場合は受け入れ環境の誤設定なので、呼び出し側はQFAILする
 * @param base 環境変数QSNAPPER_BTRFS_TEST_DIRの値
 * @param error 使用できない理由の返却先
 * @return 使用できる場合true
 */
bool btrfsTestBaseIsUsable(const QString &base, QString *error)
{
    struct statfs fsInfo;
    if (::statfs(base.toUtf8().constData(), &fsInfo) < 0) {
        *error = QStringLiteral("QSNAPPER_BTRFS_TEST_DIR cannot be stat'ed: %1: %2")
                         .arg(base, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    if (fsInfo.f_type != BTRFS_SUPER_MAGIC) {
        *error = QStringLiteral("QSNAPPER_BTRFS_TEST_DIR is not on btrfs: %1").arg(base);
        return false;
    }
    return true;
}
} // namespace

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

/**
 * @brief 木に含まれるかを、libsnapperの比較と同じ意味で判定することを検証する
 *
 * 復元計画の検証は、この結果で各entryが実際の差分に裏付けられているかを判定する
 * 中間symlinkの先や通常ファイルの下を「存在する」と判定すると、比較に現れないパスの削除や上書きを許してしまう
 * 逆に入出力エラーを「不在」と判定すると、createdの偽装が通ってしまうため、Unknownとして区別する
 */
void TestFilesystemHelpers::observePathNoFollowAtClassifiesPresenceLikeComparison()
{
    QTemporaryDir treeDir;
    QVERIFY(treeDir.isValid());

    QVERIFY(QDir().mkpath(treeDir.path() + QStringLiteral("/etc/real-dir")));
    {
        QFile file(treeDir.path() + QStringLiteral("/etc/real-dir/file"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("data");
    }
    {
        QFile file(treeDir.path() + QStringLiteral("/etc/plain"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("data");
    }
    const QString linkPath = treeDir.path() + QStringLiteral("/etc/link-to-dir");
    QVERIFY(::symlink("real-dir", linkPath.toUtf8().constData()) == 0);

    const int dirFd = ::open(treeDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    struct stat info = {};
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/real-dir/file"), &info),
             PathPresenceAt::Present);
    QVERIFY(S_ISREG(info.st_mode));

    // leafのsymlinkは辿らず、symlinkそのものとして報告する
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/link-to-dir"), &info),
             PathPresenceAt::Present);
    QVERIFY(S_ISLNK(info.st_mode));

    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/missing"), &info),
             PathPresenceAt::Absent);
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("missing/child"), &info),
             PathPresenceAt::Absent);

    // 中間のsymlinkと通常ファイルの下は、比較が辿らないため木に含まれない
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/link-to-dir/file"), &info),
             PathPresenceAt::Absent);
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/plain/child"), &info),
             PathPresenceAt::Absent);

    // 相対パスとして不正な入力と無効なfdは、存在を確定できない
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("../escape"), &info),
             PathPresenceAt::Unknown);
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("/etc/plain"), &info),
             PathPresenceAt::Unknown);
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/car\nrier"), &info),
             PathPresenceAt::Unknown);
    QCOMPARE(observePathNoFollowAt(-1, QStringLiteral("etc/plain"), &info),
             PathPresenceAt::Unknown);
    QCOMPARE(observePathNoFollowAt(dirFd, QStringLiteral("etc/plain"), nullptr),
             PathPresenceAt::Unknown);

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
 * @brief in-root解決がsymlinkループを有限回で打ち切ることを検証する
 *
 * 中間symlinkが循環してもopenat2がELOOPで失敗し、無限ループにならないこと
 */
void TestFilesystemHelpers::inRootRejectsSymlinkLoop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(rootPath));

    QVERIFY(::symlink(QStringLiteral("loop-b").toUtf8().constData(),
                      (rootPath + QStringLiteral("/loop-a")).toUtf8().constData()) == 0);
    QVERIFY(::symlink(QStringLiteral("loop-a").toUtf8().constData(),
                      (rootPath + QStringLiteral("/loop-b")).toUtf8().constData()) == 0);

    UniqueFd rootFd(::open(rootPath.toUtf8().constData(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    QVERIFY(rootFd.isValid());

    const QStringList components{QStringLiteral("loop-a"), QStringLiteral("child")};
    errno = 0;
    const UniqueFd parentFd(openParentDirectoryInRootAt(rootFd.get(), components, nullptr));
    const int openErrno = errno;
    QVERIFY(!parentFd.isValid());
    QCOMPARE(openErrno, ELOOP);
}

/**
 * @brief 復元先のwalkがnested btrfs subvolumeをEXDEVで拒否することを検証する
 *
 * nested subvolumeはmountではないためRESOLVE_NO_XDEVでは止まらない
 * 親とst_devが異なることを各段のstatxで検出できなければ、subvolumeをまたいで書き込めてしまう
 */
void TestFilesystemHelpers::nestedSubvolumeIsRejected()
{
    const QString base = QString::fromLocal8Bit(qgetenv("QSNAPPER_BTRFS_TEST_DIR"));
    if (base.isEmpty()) {
        QSKIP("QSNAPPER_BTRFS_TEST_DIR is not set");
    }
    QString baseError;
    if (!btrfsTestBaseIsUsable(base, &baseError)) {
        QFAIL(qPrintable(baseError));
    }

    NestedSubvolumeFixture fixture(base);
    if (!fixture.isValid()) {
        QFAIL(qPrintable(fixture.error()));
    }

    const QStringList components = { QStringLiteral("nested"), QStringLiteral("inside") };
    errno = 0;
    const UniqueFd parentFd(openLeafParentBeneathRoot(fixture.root(), components,
                                                      false, 0, nullptr));
    const int openErrno = errno;
    QVERIFY2(!parentFd.isValid(), "the destination walk crossed into a nested subvolume");
    QCOMPARE(openErrno, EXDEV);
}

/**
 * @brief 復元先のwalkが途中のsymlinkを追従しないことを検証する
 *
 * 中間成分のsymlinkを許すと、rootの外や別subvolumeのfileを宛先にできてしまう
 * ELOOP (O_NOFOLLOW / RESOLVE_NO_SYMLINKS) かENOTDIR (非ディレクトリ) で失敗することを求める
 */
void TestFilesystemHelpers::symlinkedMiddleComponentIsRejected()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString root = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/real")));
    QVERIFY(::symlink("real", (root + QStringLiteral("/link")).toUtf8().constData()) == 0);

    const QStringList components = { QStringLiteral("link"), QStringLiteral("inside") };
    errno = 0;
    const UniqueFd parentFd(openLeafParentBeneathRoot(root, components, false, 0, nullptr));
    const int openErrno = errno;
    QVERIFY2(!parentFd.isValid(), "the destination walk followed a middle symlink");
    QVERIFY(openErrno == ELOOP || openErrno == ENOTDIR);
}

/**
 * @brief 復元先のwalkが通常のnested directoryを開けることを検証する
 *
 * mount境界の拒否が正当なディレクトリまで巻き込まないことを確認する
 */
void TestFilesystemHelpers::plainNestedDirectoryIsOpened()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString root = tempDir.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/plain")));

    const QStringList components = { QStringLiteral("plain"), QStringLiteral("file") };
    QByteArray leafName;
    const UniqueFd parentFd(openLeafParentBeneathRoot(root, components, false, 0, &leafName));
    QVERIFY2(parentFd.isValid(), std::strerror(errno));
    QCOMPARE(leafName, QByteArrayLiteral("file"));

    struct stat parentInfo;
    QCOMPARE(::fstat(parentFd.get(), &parentInfo), 0);
    QVERIFY(S_ISDIR(parentInfo.st_mode));
}

/**
 * @brief 絶対パス起点のwalkがsameMountOnly=falseでnested subvolumeを越えられることを検証する
 *
 * snapshot既存木の読み取りなど、subvolume越えが正当な用途では境界を課さない
 */
void TestFilesystemHelpers::nestedSubvolumeAllowedWithoutSameMountOnly()
{
    const QString base = QString::fromLocal8Bit(qgetenv("QSNAPPER_BTRFS_TEST_DIR"));
    if (base.isEmpty()) {
        QSKIP("QSNAPPER_BTRFS_TEST_DIR is not set");
    }
    QString baseError;
    if (!btrfsTestBaseIsUsable(base, &baseError)) {
        QFAIL(qPrintable(baseError));
    }

    NestedSubvolumeFixture fixture(base);
    if (!fixture.isValid()) {
        QFAIL(qPrintable(fixture.error()));
    }

    QStringList components;
    QVERIFY(splitAbsolutePath(fixture.root() + QStringLiteral("/nested"), &components));
    if (components.isEmpty()) {
        QFAIL("the test path unexpectedly had no components");
    }

    const UniqueFd subvolumeFd(openDirectoryChainNoFollow(components, components.size(), false, 0));
    const int openErrno = errno;
    QVERIFY2(subvolumeFd.isValid(), std::strerror(openErrno));

    struct stat rootInfo;
    struct stat subvolumeInfo;
    QCOMPARE(::stat(fixture.root().toUtf8().constData(), &rootInfo), 0);
    QCOMPARE(::fstat(subvolumeFd.get(), &subvolumeInfo), 0);
    QVERIFY2(subvolumeInfo.st_dev != rootInfo.st_dev,
             "the fixture did not produce a distinct st_dev for the nested subvolume");
}

/**
 * @brief 途中にmountポイントがあるパスを、不在ではなく判定不能として扱うことを検証する
 *
 * 不在とすると、mountの先にある実在のファイルを対象にした偽の [deleted] エントリが検証を通ってしまう
 * mountポイントそのもの (末尾の成分) は存在として扱う
 */
void TestFilesystemHelpers::observePathNoFollowAtRejectsMountBoundaries()
{
    struct stat rootInfo;
    struct stat procInfo;
    QCOMPARE(::stat("/", &rootInfo), 0);
    if (::stat("/proc/version", &procInfo) != 0 || procInfo.st_dev == rootInfo.st_dev) {
        QSKIP("/proc is not a separate mount in this environment");
    }

    const int rootFd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);

    struct stat observed;
    errno = 0;
    const PathPresenceAt beyondMount = observePathNoFollowAt(rootFd, QStringLiteral("proc/version"), &observed);
    const int beyondError = errno;
    const PathPresenceAt mountPoint = observePathNoFollowAt(rootFd, QStringLiteral("proc"), &observed);
    ::close(rootFd);

    QCOMPARE(beyondMount, PathPresenceAt::Unknown);
    QCOMPARE(beyondError, EXDEV);
    QCOMPARE(mountPoint, PathPresenceAt::Present);
}
