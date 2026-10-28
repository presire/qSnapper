/**
 * @file tst_filesystemhelpers_dirfd.cpp
 * @brief dirfd相対のソース解決と復元先walkの単体テスト
 *
 * dirfdを起点にしたstat・読み出し・不在確認が、絶対パスと".."成分を拒否し、中間symlinkによってdirfdの外へ解決されないことを検証する
 *
 * openat2のRESOLVE_IN_ROOTを使う経路では、絶対symlinkと".."をroot内へ丸め、復元元のsnapshotをmount前後で同じように読めることも確認する
 *
 * 復元先walkではnested btrfs subvolumeとsymlinkの中間成分を境界として拒否し、mount境界の先を不在と誤判定しないことを確認する
 *
 * 呼び出し元 : tests/unit/tst_filesystemhelpers.hで宣言するTestFilesystemHelpersのslot
 * 依存 : qsnapper::securityのsafeLstatAt・safeOpenRegularFileReadAt・
 *        safeReadLinkNoFollowAt・isConfirmedAbsentAt・observePathNoFollowAt・
 *        openLeafParentBeneathRoot・openParentDirectoryInRootAt・
 *        openDirectoryChainNoFollow・safeRenamePathNoFollowBeneathRootと、
 *        ObservedDirectory・DirectoryChainOwnership・PathPresenceAtなどの型
 *        解決にはopenat2のRESOLVE_IN_ROOTとRESOLVE_NO_XDEV、
 *        subvolumeの作成にはBTRFS_IOC_SUBVOL_CREATEを使う
 * 前提 : 実システムのファイルは変更せず、一時ディレクトリだけで完結する
 *        ただしmount境界の観測は/procが別mountであることを利用する
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>
#include "filesystemhelpers.h"
#include "filesystemhelpers_internal.h"
#include "tst_filesystemhelpers.h"

namespace {
/**
 * @class NestedSubvolumeFixture
 * @brief 復元先walkのテストに使うnested btrfs subvolume
 *
 * 環境変数QSNAPPER_BTRFS_TEST_DIRが指すbtrfs上のベースディレクトリに、mountを伴わないsubvolumeをルート直下へ作る
 * subvolumeはmount境界ではないためRESOLVE_NO_XDEVでは止まらず、st_devの比較でしか検出できないことを確かめるために使用する
 *
 * テストが失敗しても残さないよう、破棄時に内側のfileとsubvolumeを順に消す
 * 作成に失敗した理由はerror()で取得でき、呼び出し側がQFAILする
 *
 * m_root : テストごとにpidを付けて衝突を避ける一時ルート
 * m_subvolume : ルート直下に作るnested subvolume
 * m_insideFile : subvolume内に作る通常ファイル
 * m_error : 作成に失敗した理由、成功時は空文字列
 * m_valid : subvolumeとfileを作成できたか
 */
class NestedSubvolumeFixture
{
public:
    /**
     * @brief テスト用ルート、nested subvolume、subvolume内のfileを作成する
     * @param base btrfs上のベースディレクトリ
     *
     * 失敗しても例外を投げずm_errorへ理由を残すため、呼び出し側がisValid()を確認してQFAILする
     * @note 作成に使うfdはUniqueFdが保持し、成功しても失敗してもこのメソッドを抜ける時点で閉じられる
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
     *
     * コンストラクタが途中で失敗した場合も呼ばれるため、存在しないpathの削除失敗は無視する
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
     * コピーを許すと同じsubvolumeとfileを2つのインスタンスが所有し、それぞれのデストラクタが2重に削除してしまうため
     */
    NestedSubvolumeFixture(const NestedSubvolumeFixture &) = delete;

    /**
     * @brief コピー代入を禁止する
     *
     * 代入で所有権が移ったように見えると、元と先の両方が同じsubvolumeとfileを削除してしまうため
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
    QString m_root;        // テストごとの一時ルート、root-<pid>形式
    QString m_subvolume;   // ルート直下に作るnested subvolume
    QString m_insideFile;  // subvolume内に作る通常ファイル
    QString m_error;       // 作成に失敗した理由、成功時は空文字列
    bool m_valid = false;  // subvolumeとfileを作成できたか
};

/**
 * @brief btrfsのテスト基盤が使えるかを確認する
 *
 * 環境変数の設定有無は呼び出し側が判定する
 * 設定済みなのに使えない場合は受け入れ環境の誤設定なので、呼び出し側はQFAILする
 *
 * @param base 環境変数QSNAPPER_BTRFS_TEST_DIRの値
 * @param error 使用できない理由の返却先
 * @return 使用できる場合true
 * @note statfsに失敗した場合はerrnoを理由に含める
 * @note btrfs以外のファイルシステムではsubvolumeを作れないためfalseを返す
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
/**
 * @brief safeLstatAtがdirfdを起点に相対パスを解決することを検証する
 *
 * 通常ファイルとleafのsymlinkを用意し、symlinkは展開せず自身の属性を返すことを確認する
 */
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

    // dirFdを起点に相対パスを解決し、通常ファイルとして報告する
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

/**
 * @brief safeLstatAtがdirfdの外を指す入力と不正なパスを拒否することを検証する
 *
 * 絶対パス、".."、"."、NULを含むパス、空文字列を渡し、いずれもEINVALで拒否されることを確認する
 */
void TestFilesystemHelpers::safeLstatAtRejectsAbsoluteAndTraversalPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    struct stat st;
    // 絶対パスと".."成分はdirfdの外を指せるため、解決前に拒否する
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

/**
 * @brief safeOpenRegularFileReadAtがsymlinkのleafと通常ファイル以外を拒否することを検証する
 *
 * leafのsymlinkはO_NOFOLLOWでELOOPになり、directoryは通常ファイルとして開けない
 * 絶対パスと".."成分とNULを含むパスは、解決前にEINVALで拒否される
 */
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

    // leafのsymlinkを辿るとdirfdの外のfileを読むため、O_NOFOLLOWで拒否する
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("link.txt")) < 0);
    QCOMPARE(errno, ELOOP);

    // directoryは通常ファイルとして開けない
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(dirFd, QStringLiteral("subdir")) < 0);

    // 絶対パスと".."成分とNULを含むパスは、解決前にEINVALで拒否される
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

/**
 * @brief safeReadLinkNoFollowAtがdirfdを起点にsymlinkのtargetを読むことを検証する
 *
 * 相対targetを持つsymlinkを用意し、linkの内容がtargetと一致することを確認する
 */
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

/**
 * @brief safeReadLinkNoFollowAtが不正なパスとsymlink以外のleafを拒否することを検証する
 *
 * 絶対パスと".."と"."と空文字列とNULを含むパスはEINVALになる
 * symlink以外のleafはreadlinkatの仕様によりEINVALで失敗する
 */
void TestFilesystemHelpers::safeReadLinkNoFollowAtRejectsInvalidPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QVERIFY(QDir().mkpath(tempDir.path() + QStringLiteral("/sub")));

    const int dirFd = ::open(tempDir.path().toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(dirFd >= 0);

    QByteArray observedTarget;
    // 絶対パスと".."と"."と空文字列とNULを含むパスは、検証段階でEINVALで拒否される
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

    // symlink以外のleafはreadlinkatがEINVALで拒否する
    errno = 0;
    QVERIFY(!safeReadLinkNoFollowAt(dirFd, QStringLiteral("sub"), &observedTarget));
    QCOMPARE(errno, EINVAL);

    QVERIFY(::close(dirFd) == 0);
}

/**
 * @brief 復元元snapshotに存在するパスを不在と誤判定しないことを検証する
 *
 * 復元のcreated entryはlive側からの削除として実行されるため、ここでtrueを返すと復元元に存在するパスが削除される
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
 * 「不在」と「確認できなかった」を混同すると、安全側に倒すfail-closedが成立しない
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

    // ENOENTは正当なcreated entryであり、このまま削除へ進む
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("etc/created-after-snapshot")));
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("absent-top-level")));

    // ENOTDIRは親成分が通常ファイルなので、配下は確定的に存在し得ない
    QVERIFY(isConfirmedAbsentAt(dirFd, QStringLiteral("blocking/child")));

    // 相対パス検証で弾かれる入力はEINVALであり、不在を確認できなかった場合に当たる
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
 * fstatat()のAT_SYMLINK_NOFOLLOWはleafにしか効かないため、中間成分の解決をfstatat()に任せると固定したdirfdの外へ解決され得る
 * symlinkに当たった場合は、「不在を確認できなかった」として削除を許可してはならない
 */
void TestFilesystemHelpers::isConfirmedAbsentAtRefusesIntermediateSymlinks()
{
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());

    QVERIFY(QDir().mkpath(sourceDir.path() + QStringLiteral("/etc")));

    // 相対target、絶対target、存在するディレクトリへのtargetの3種を用意する
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

    // いずれも不在を確認できていないため、不在判定としてはfalseでなければならない
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
// dirfd相対ソース解決の中間成分のin-rootハードニング、RESOLVE_IN_ROOT
// ============================================================================

/**
 * @brief 絶対symlinkの中間成分がdirfdの外へ解決されないことを検証する
 *
 * 絶対symlink targetと同一の絶対位置をdirfd内に再現し、
 * 読み出した内容がINSIDEであり、拒否ではなくin-root解決になっていることまで確認する
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

    // in-root解決なら参照されるよう、symlink targetと同じ絶対位置をroot内に再現する
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
 * RESOLVE_IN_ROOTでは"/.."が"/"に留まるため、root直下のsymlinkが
 * ".."を含んでもdirfdの外へ出ない
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
 * @brief 正当な相対中間symlinkを3つの読み取りヘルパーが解決できることを検証する
 *
 * snapshot内の"bin -> usr/bin"のような相対symlinkを、
 * 従来通りsnapshot内のtargetへ解決する
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

    // leafのsymlinkはO_NOFOLLOWによりELOOPで拒否される
    errno = 0;
    QVERIFY(safeOpenRegularFileReadAt(rootFd, QStringLiteral("bin/leaflink")) < 0);
    QCOMPARE(errno, ELOOP);

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief 絶対と相対の中間symlinkを含む正当なsnapshotから復元読み取りできることを検証する
 *
 * 実際のsnapshot構成を模した木に対し、
 * safeLstatAt・safeOpenRegularFileReadAt・safeReadLinkNoFollowAtが
 * snapshot自身の内容とメタデータを返すことを通しで確認する
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

    // 通常ファイルをsnapshotの内容どおりに読み出せる
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

    // directoryは通常ファイルとして開かず、属性だけを読む
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("usr/share"), &st));
    QVERIFY(S_ISDIR(st.st_mode));

    // 絶対中間symlinkはRESOLVE_IN_ROOTによりsnapshot内の/runへ解決される
    {
        const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("var/run/file"));
        QVERIFY(fd >= 0);
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("in-root run"));
    }

    // 相対中間symlinkはsnapshot内のusr/binへ解決される
    {
        const int fd = safeOpenRegularFileReadAt(rootFd, QStringLiteral("bin/tool"));
        QVERIFY(fd >= 0);
        QFile file;
        QVERIFY(file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
        QCOMPARE(file.readAll(), QByteArray("tool-content"));
    }

    // 中間symlinkを辿ってもleafのsymlinkは展開せず、symlink自身として読める
    QVERIFY(safeLstatAt(rootFd, QStringLiteral("bin/tool-link"), &st));
    QVERIFY(S_ISLNK(st.st_mode));
    QByteArray target;
    QVERIFY(safeReadLinkNoFollowAt(rootFd, QStringLiteral("bin/tool-link"), &target));
    QCOMPARE(target, QByteArray("tool"));

    // 中間symlink自身をleafとして渡すと、そのtargetを取得できる
    QVERIFY(safeReadLinkNoFollowAt(rootFd, QStringLiteral("bin"), &target));
    QCOMPARE(target, QByteArray("usr/bin"));

    QVERIFY(::close(rootFd) == 0);
}

/**
 * @brief in-root解決がsymlinkループを有限回で打ち切ることを検証する
 *
 * 中間symlinkが循環してもopenat2がELOOPで失敗し、無限ループにならないことを確認する
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
 * ELOOPまたはENOTDIRで失敗することを求める
 * O_NOFOLLOWとRESOLVE_NO_SYMLINKSがsymlinkを拒否し、
 * ディレクトリ以外の成分はENOTDIRになる
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
 * 不在とすると、mountの先にある実在のfileを対象にした偽の[deleted]エントリが検証を通ってしまう
 * mountポイント自体は末尾の成分として存在扱いにする
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

namespace {
constexpr uid_t DIRECTORY_OWNER_UID = 1000;  // 検査するディレクトリの所有者のUID
constexpr gid_t DIRECTORY_OWNER_GID = 1000;  // 所有者専用のグループとして扱うGID
constexpr gid_t SHARED_GROUP_GID = 3000;     // 所有者と他の利用者が共に属する共有グループのGID

/**
 * @brief 所有者がDIRECTORY_OWNER_UIDでグループがDIRECTORY_OWNER_GIDの組だけを、所有者専用のグループとして扱う判定
 *
 * テストを実行するホストのpasswdとgroupの登録内容に結果が左右されないよう、isUserPrivateGroupの代わりに使う
 * @param owner ディレクトリの所有者のUID
 * @param group ディレクトリのグループのGID
 * @return 組がDIRECTORY_OWNER_UIDとDIRECTORY_OWNER_GIDの場合true
 */
bool onlyOwnerGroupIsPrivate(uid_t owner, gid_t group)
{
    return owner == DIRECTORY_OWNER_UID && group == DIRECTORY_OWNER_GID;
}

/**
 * @brief passwdやgroupを引けない場合を模し、どの組も所有者専用のグループとして扱わない判定
 * @param owner 判定に使わない
 * @param group 判定に使わない
 * @return 常にfalse
 */
bool noGroupIsPrivate(uid_t owner, gid_t group)
{
    Q_UNUSED(owner)
    Q_UNUSED(group)
    return false;
}

/**
 * @brief 所有者と所有グループ以外に名前付き利用者のエントリを持つ拡張access ACLを返す
 *
 * 所有者以外の利用者へ書き込みを許すACLを、modeのgroupのビットに頼らずに作るために使う
 * @note 値をxattrのstruct posix_acl_xattr_headerとentryの並びにlittle-endianで組み立てる
 * @return system.posix_acl_accessのxattr値
 */
QByteArray extendedAccessAcl()
{
    const auto entry = [](quint16 tag, quint16 perm, quint32 id) {
        QByteArray bytes;
        const quint64 fields[] = {tag, perm, id};
        const int widths[] = {2, 2, 4};
        for (int field = 0; field < 3; ++field) {
            for (int i = 0; i < widths[field]; ++i) {
                bytes.append(static_cast<char>((fields[field] >> (8 * i)) & 0xff));
            }
        }
        return bytes;
    };
    QByteArray value;
    value.append(static_cast<char>(2)).append(3, '\0');
    value.append(entry(0x01, 7, 0xffffffffU));
    value.append(entry(0x02, 7, 2000));
    value.append(entry(0x04, 7, 0xffffffffU));
    value.append(entry(0x10, 7, 0xffffffffU));
    value.append(entry(0x20, 5, 0xffffffffU));
    return value;
}

/**
 * @class ObservedChainFixture
 * @brief 実在するディレクトリのfdと、所有者とmodeを書き換えたfstat結果の並びを作る
 *
 * 所有者の変更にはrootが要るため、fstat結果だけを書き換えて非rootでも所有権の集計を試せるようにする
 * 拡張ACLの判定はfdから読むため、ACLを指定した要素には実際にxattrを設定する
 *
 * m_root : 要素ごとのディレクトリを置く一時ディレクトリ
 * m_fds : 開いたディレクトリのfd、デストラクタで閉じる
 */
class ObservedChainFixture
{
public:
    ~ObservedChainFixture()
    {
        for (const int fd : m_fds) {
            ::close(fd);
        }
    }

    /**
     * @brief 「uid:gid:8進のmode」または末尾に「:acl」を付けた指定から並びを作る
     * @param specs 「/」に近い順の指定
     * @param chainOut 作った並びの返却先
     * @return 作れた場合true、ACLを設定できないファイルシステムの場合はfalse
     */
    bool build(const QStringList &specs, std::vector<qsnapper::security::ObservedDirectory> *chainOut)
    {
        if (!m_root.isValid()) {
            return false;
        }
        for (qsizetype index = 0; index < specs.size(); ++index) {
            const QStringList fields = specs.at(index).split(QLatin1Char(':'));
            const QString path = m_root.filePath(QString::number(index));
            if (::mkdir(path.toUtf8().constData(), 0700) != 0) {
                return false;
            }
            const int fd = ::open(path.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) {
                return false;
            }
            m_fds.push_back(fd);
            if (fields.size() > 3 && fields.at(3) == QLatin1String("acl")) {
                const QByteArray acl = extendedAccessAcl();
                if (::fsetxattr(fd, "system.posix_acl_access", acl.constData(),
                                static_cast<size_t>(acl.size()), 0) != 0) {
                    return false;
                }
            }
            qsnapper::security::ObservedDirectory observed {fd, {}};
            if (::fstat(fd, &observed.info) != 0) {
                return false;
            }
            observed.info.st_uid = fields.at(0).toUInt();
            observed.info.st_gid = fields.at(1).toUInt();
            observed.info.st_mode = S_IFDIR | static_cast<mode_t>(fields.at(2).toUInt(nullptr, 8));
            chainOut->push_back(observed);
        }
        return true;
    }

private:
    QTemporaryDir m_root;    // 要素ごとのディレクトリを置く一時ディレクトリ
    std::vector<int> m_fds;  // 開いたディレクトリのfd、デストラクタで閉じる
};

/**
 * @brief 所有権の集計結果をtrusted、principal、conflict、unknownの名前で返す
 *
 * principalはチェーン全体を単一の所有者として扱える状態を指す
 * @param ownership 集計した状態
 * @return trusted、principal、conflict、unknownのどれか
 */
QString chainOwnershipKind(const qsnapper::security::DirectoryChainOwnership &ownership)
{
    if (ownership.conflict) {
        return QStringLiteral("conflict");
    }
    if (ownership.trusted) {
        return QStringLiteral("trusted");
    }
    return ownership.principalKnown() ? QStringLiteral("principal") : QStringLiteral("unknown");
}

/**
 * @brief ディレクトリを作り、所有者とmodeを設定する
 * @param path 作るディレクトリ
 * @param owner 所有者のuid
 * @param group 所有グループのgid
 * @param mode 設定するmode、umaskの影響を受けないようchmodで設定する
 * @note mkdirに渡す0700はumaskで削られ得るため、最終的なmodeはchmodで上書きする
 * @return 全て成功した場合true
 */
bool makeOwnedDirectory(const QString &path, uid_t owner, gid_t group, mode_t mode)
{
    const QByteArray encoded = path.toUtf8();
    return ::mkdir(encoded.constData(), 0700) == 0 && ::chown(encoded.constData(), owner, group) == 0
           && ::chmod(encoded.constData(), mode) == 0;
}
} // namespace

/**
 * @brief 所有者専用グループの判定が、名前・GID・メンバー一覧をどう組み合わせるかを検証する
 *
 * 所有者名とグループ名が一致し、所有者の主グループであり、本人以外のメンバーを持たない場合だけ
 * 所有者専用のグループと認める
 */
void TestFilesystemHelpers::userPrivateGroupEntryRequiresExclusiveMembership_data()
{
    QTest::addColumn<QString>("ownerName");
    QTest::addColumn<uint>("ownerGid");
    QTest::addColumn<QString>("groupName");
    QTest::addColumn<uint>("groupGid");
    QTest::addColumn<QStringList>("members");
    QTest::addColumn<bool>("expected");

    // 名前とGIDが一致し主グループであれば、明示メンバーがいなくても専用とみなす
    QTest::newRow("upg-empty") << "alice" << 1000u << "alice" << 1000u << QStringList {} << true;
    // 本人だけが明示メンバーでも、他の利用者を追加していないため専用とみなす
    QTest::newRow("upg-self-member") << "alice" << 1000u << "alice" << 1000u << QStringList {"alice"} << true;
    // 他の利用者が明示メンバーなら、本人しか使えないグループではなくなる
    QTest::newRow("upg-other-member") << "alice" << 1000u << "alice" << 1000u << QStringList {"bob"} << false;
    // 本人を含んでいても他の利用者を追加していれば専用ではない
    QTest::newRow("upg-self-and-other")
        << "alice" << 1000u << "alice" << 1000u << QStringList {"alice", "bob"} << false;
    // 主グループがusersのような共有グループなら、名前の一致だけでは専用と認めない
    QTest::newRow("shared-primary-users") << "alice" << 100u << "users" << 100u << QStringList {} << false;
    // GIDが同じでもグループ名が所有者名と違えば、個人用のグループとはみなさない
    QTest::newRow("name-mismatch") << "alice" << 1000u << "team" << 1000u << QStringList {} << false;
    // 名前が一致しても所有者の主グループでなければ、別の利用者と共有している可能性が残る
    QTest::newRow("not-primary") << "alice" << 100u << "alice" << 1000u << QStringList {} << false;
}

void TestFilesystemHelpers::userPrivateGroupEntryRequiresExclusiveMembership()
{
    QFETCH(QString, ownerName);
    QFETCH(uint, ownerGid);
    QFETCH(QString, groupName);
    QFETCH(uint, groupGid);
    QFETCH(QStringList, members);
    QFETCH(bool, expected);

    QByteArray ownerNameBytes = ownerName.toUtf8();
    QByteArray groupNameBytes = groupName.toUtf8();
    std::vector<QByteArray> memberBytes;
    for (const QString &member : members) {
        memberBytes.push_back(member.toUtf8());
    }
    std::vector<char *> memberPointers;
    for (QByteArray &member : memberBytes) {
        memberPointers.push_back(member.data());
    }
    memberPointers.push_back(nullptr);

    struct passwd owner = {};
    owner.pw_name = ownerNameBytes.data();
    owner.pw_uid = DIRECTORY_OWNER_UID;
    owner.pw_gid = ownerGid;
    struct group group = {};
    group.gr_name = groupNameBytes.data();
    group.gr_gid = groupGid;
    group.gr_mem = memberPointers.data();

    QCOMPARE(qsnapper::security::isUserPrivateGroupEntry(owner, group), expected);
}

/**
 * @brief passwdとgroupの走査が、共有するアカウントや読めない入力をどう分類するかを検証する
 *
 * 所有者の主グループに他の利用者がいるか、同じGIDのgroupが重複していないかを
 * 行の長さや入力の可否によらず判定できることを確かめる
 */
void TestFilesystemHelpers::localAccountScanDetectsSharing_data()
{
    using qsnapper::security::LocalScanResult;
    QTest::addColumn<bool>("passwdFormat");
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<bool>("unreadable");
    QTest::addColumn<int>("expected");

    // 共有者が100行目より後ろに現れても見落とさないよう、無関係な利用者で埋める
    QByteArray unrelatedUsers;
    for (int index = 0; index < 100; ++index) {
        unrelatedUsers += QByteArray("user") + QByteArray::number(index) + ":x:" + QByteArray::number(5000 + index)
                          + ":" + QByteArray::number(5000 + index) + "::/home/u:/bin/sh\n";
    }
    // sysconfが返す初期バッファ長に収まらない長さにし、ERANGEで広げても読み続けることを確かめる
    const QByteArray longGecos(200 * 1024, 'g');

    // 所有者と同じGIDの利用者が本人だけなら、共有していないと判定する
    QTest::newRow("passwd-exclusive")
        << true << QByteArray("alice:x:1000:1000::/home/alice:/bin/sh\nbob:x:2000:2000::/home/bob:/bin/sh\n")
        << false << int(LocalScanResult::Exclusive);
    // 100行の後ろに現れる共有者も、打ち切らずに検出する
    QTest::newRow("passwd-sharer-late")
        << true << unrelatedUsers + "carol:x:3000:1000::/home/carol:/bin/sh\n" << false
        << int(LocalScanResult::Shared);
    // 初期バッファ長を超えるGECOS行の直後に続く共有者も検出する
    QTest::newRow("passwd-long-line")
        << true
        << QByteArray("dave:x:4000:4000:") + longGecos + ":/home/dave:/bin/sh\ncarol:x:3000:1000::/home/carol:/bin/sh\n"
        << false << int(LocalScanResult::Shared);
    // 共有者の行自体が初期バッファ長を超えても検出する
    QTest::newRow("passwd-long-line-sharer")
        << true << QByteArray("carol:x:3000:1000:") + longGecos + ":/home/carol:/bin/sh\n" << false
        << int(LocalScanResult::Shared);
    // passwdを読めない場合は、共有の有無を判定できないためFailedとする
    QTest::newRow("passwd-unreadable") << true << QByteArray() << true << int(LocalScanResult::Failed);
    // 所有者のGIDに対応するgroupが1つだけなら、共有するgroupは無い
    QTest::newRow("group-single") << false << QByteArray("alice:x:1000:\nteam:x:3000:bob\n") << false
                                  << int(LocalScanResult::Exclusive);
    // 同じGIDのgroupが2つあれば、所有者専用でないgroupを経由して共有している可能性がある
    QTest::newRow("group-duplicate-gid") << false << QByteArray("alice:x:1000:\nteam:x:1000:bob\n") << false
                                         << int(LocalScanResult::Shared);
    // 所有者のGIDに対応するgroupが無ければ、所有者専用グループの判定ができない
    QTest::newRow("group-missing") << false << QByteArray("team:x:3000:bob\n") << false
                                   << int(LocalScanResult::Failed);
    // groupを読めない場合も、判定できないためFailedとする
    QTest::newRow("group-unreadable") << false << QByteArray() << true << int(LocalScanResult::Failed);
}

void TestFilesystemHelpers::localAccountScanDetectsSharing()
{
    QFETCH(bool, passwdFormat);
    QFETCH(QByteArray, content);
    QFETCH(bool, unreadable);
    QFETCH(int, expected);

    // 書き込み専用で開いたストリームは読み取りがEBADFで失敗するため、読めないファイルの代わりに使う
    QByteArray storage = unreadable ? QByteArray(64, '\0') : content;
    FILE *stream = ::fmemopen(storage.data(), static_cast<size_t>(storage.size()), unreadable ? "w" : "r");
    QVERIFY(stream);
    const qsnapper::security::LocalScanResult result =
        passwdFormat ? qsnapper::security::scanPasswdForOtherPrimaryMember(stream, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID)
                     : qsnapper::security::scanGroupForDuplicateGid(stream, DIRECTORY_OWNER_GID);
    ::fclose(stream);

    QCOMPARE(int(result), expected);
}

/**
 * @brief ディレクトリの所有権の連なりから、名前を委ねてよいか、単一の所有者に限定できるかを検証する
 *
 * rootから始まるtrustedな連なりと、途中で切り替わるprincipalだけを許可する
 * 共有グループ、他の利用者への書込み、実効権限を増やすACL、所有者の混在はconflictとする
 */
void TestFilesystemHelpers::chainOwnershipRefusesDelegatedNames_data()
{
    QTest::addColumn<QStringList>("chain");
    QTest::addColumn<bool>("ownerGroupIsPrivate");
    QTest::addColumn<QString>("expected");

    // root所有の連なりは、名前を委ねても書き換えられないためtrustedとする
    QTest::newRow("root-0755") << QStringList {"0:0:0755"} << true << "trusted";
    // rootから単一の所有者へ切り替わる連なりは、その所有者に限定できるためprincipalとする
    QTest::newRow("root-then-principal-0755") << QStringList {"0:0:0755", "1000:1000:0755"} << true << "principal";
    // グループが所有者専用なら、groupとotherに書込みが無い限り他の利用者は書き換えられない
    QTest::newRow("principal-0700") << QStringList {"1000:1000:0700"} << true << "principal";
    QTest::newRow("principal-0775-upg") << QStringList {"1000:1000:0775"} << true << "principal";
    QTest::newRow("principal-01775-upg") << QStringList {"1000:1000:01775"} << true << "principal";
    // 主グループが所有者専用でなければ、グループ経由で他の利用者が書き換えられる
    QTest::newRow("principal-0775-non-upg") << QStringList {"1000:100:0775"} << true << "conflict";
    QTest::newRow("principal-0770-team") << QStringList {"1000:3000:0770"} << true << "conflict";
    QTest::newRow("principal-01770-team") << QStringList {"1000:3000:01770"} << true << "conflict";
    // 所有者専用グループでも、otherへの書込みや全員への書込みは他の利用者に名前を委ねることになる
    QTest::newRow("principal-0757") << QStringList {"1000:1000:0757"} << true << "conflict";
    QTest::newRow("principal-0777-upg") << QStringList {"1000:1000:0777"} << true << "conflict";
    // 名前付き利用者に書込みを許すACLは、modeに現れない共有経路になる
    QTest::newRow("principal-0775-upg-acl") << QStringList {"1000:1000:0775:acl"} << true << "conflict";
    // ACLはあるが実効権限を増やさない場合は、共有経路にならない
    QTest::newRow("principal-0755-acl") << QStringList {"1000:1000:0755:acl"} << true << "principal";
    // 2段目で共有グループが現れると、その親の下では名前を守れない
    QTest::newRow("second-level-0770") << QStringList {"1000:1000:0755", "1000:3000:0770"} << true << "conflict";
    // 所有者が途中で別の利用者に変わると、単一の所有者に限定できない
    QTest::newRow("second-level-other-owner")
        << QStringList {"1000:1000:0755", "2000:2000:0755"} << true << "conflict";
    // rootのstickyな共有ディレクトリも、他の利用者が名前を追加できるためconflictとする
    QTest::newRow("root-shared-1777") << QStringList {"0:0:01777"} << true << "conflict";
    // 所有者専用グループの判定が失敗したら、共有を否定できないためconflictとする
    QTest::newRow("upg-lookup-fails") << QStringList {"1000:1000:0775"} << false << "conflict";
}

/**
 * @brief 組み立てた所有権の連なりを集計し、委譲の可否を期待値と比べる
 */
void TestFilesystemHelpers::chainOwnershipRefusesDelegatedNames()
{
    QFETCH(QStringList, chain);
    QFETCH(bool, ownerGroupIsPrivate);
    QFETCH(QString, expected);

    qsnapper::security::detail::setUserPrivateGroupCheckForTesting(ownerGroupIsPrivate ? onlyOwnerGroupIsPrivate
                                                                               : noGroupIsPrivate);
    ObservedChainFixture fixture;
    std::vector<qsnapper::security::ObservedDirectory> observed;
    QVERIFY(fixture.build(chain, &observed));

    qsnapper::security::DirectoryChainOwnership ownership;
    for (const qsnapper::security::ObservedDirectory &directory : observed) {
        qsnapper::security::accumulateChainOwnership(ownership, directory.fd, directory.info);
    }

    QCOMPARE(chainOwnershipKind(ownership), expected);
}

/**
 * @brief NSSで引けないuidとgidの組を、共有の可能性を否定できないものとして扱うことを検証する
 *
 * テスト用のフックを設定せず、実際のNSSで登録されていないuidとgidを探して使う
 * @note 4000000000番台は通常のローカルアカウントに割り当てられないため、未登録のIDを探す起点にする
 */
void TestFilesystemHelpers::userPrivateGroupRejectsUnresolvableIds()
{
    // ホストのNSSに登録されていないIDを、高い値から1000個の範囲で探す
    std::vector<char> buffer(64 * 1024);
    uid_t unregisteredUid = 0;
    for (uid_t candidate = 4000000000U; candidate > 4000000000U - 1000; --candidate) {
        struct passwd entry;
        struct passwd *result = nullptr;
        if (::getpwuid_r(candidate, &entry, buffer.data(), buffer.size(), &result) == 0 && !result) {
            unregisteredUid = candidate;
            break;
        }
    }
    gid_t unregisteredGid = 0;
    for (gid_t candidate = 4000000000U; candidate > 4000000000U - 1000; --candidate) {
        struct group entry;
        struct group *result = nullptr;
        if (::getgrgid_r(candidate, &entry, buffer.data(), buffer.size(), &result) == 0 && !result) {
            unregisteredGid = candidate;
            break;
        }
    }
    QVERIFY(unregisteredUid != 0);
    QVERIFY(unregisteredGid != 0);

    QVERIFY(!qsnapper::security::isUserPrivateGroup(unregisteredUid, ::getgid()));
    QVERIFY(!qsnapper::security::isUserPrivateGroup(::getuid(), unregisteredGid));
}

/**
 * @brief rootから対象までの祖先の連なりが、名前の変更を許せる所有権かを検証する
 *
 * root所有の区間と、途中から単一の所有者へ切り替わる区間だけを許可する
 * 共有されたstickyディレクトリ、共有グループ、複数の所有者が混ざる連なりは拒否する
 */
void TestFilesystemHelpers::ancestorChainOwnershipCoversRootPath_data()
{
    QTest::addColumn<QStringList>("ancestors");
    QTest::addColumn<bool>("allowed");

    // root所有の区間で終わる連なりは、rootが名前を守るため許可する
    QTest::newRow("standard") << QStringList {"0:0:0755", "0:0:0755"} << true;
    // 途中から所有者1000に切り替わる連なりは、その所有者に限定できる
    QTest::newRow("user-home-sub") << QStringList {"0:0:0755", "0:0:0755", "1000:1000:0700", "1000:1000:0755"}
                                   << true;
    // 所有者専用グループの0775は、他の利用者が書き換えられないため許可する
    QTest::newRow("user-home-upg") << QStringList {"0:0:0755", "0:0:0755", "1000:1000:0775", "1000:1000:0755"}
                                   << true;
    // root所有のstickyな01777は、他の利用者が名前を追加できるため拒否する
    QTest::newRow("shared-tmp") << QStringList {"0:0:0755", "0:0:01777", "1000:1000:0700"} << false;
    // 共有グループに書込みを許す0770は、グループ経由で名前を動かせるため拒否する
    QTest::newRow("team-0770") << QStringList {"0:0:0755", "1000:3000:0770", "1000:1000:0755"} << false;
    // 1000より前に別の所有者2000が現れると、単一の所有者に限定できない
    QTest::newRow("owner-break") << QStringList {"0:0:0755", "2000:2000:0755", "1000:1000:0700"} << false;
    // 所有者1000の下にrootが現れると、所有者が戻るため限定できない
    QTest::newRow("root-under-user") << QStringList {"0:0:0755", "1000:1000:0755", "0:0:0755"} << false;
}

/**
 * @brief 祖先の所有権を集計し、名前の変更を許せるかを確かめる
 *
 * 与えた祖先をrootに近い順に組み立て、共有された親が1つでもあれば拒否されることを確認する
 */
void TestFilesystemHelpers::ancestorChainOwnershipCoversRootPath()
{
    QFETCH(QStringList, ancestors);
    QFETCH(bool, allowed);

    qsnapper::security::detail::setUserPrivateGroupCheckForTesting(onlyOwnerGroupIsPrivate);
    ObservedChainFixture fixture;
    std::vector<qsnapper::security::ObservedDirectory> observed;
    QVERIFY(fixture.build(ancestors, &observed));

    const qsnapper::security::DirectoryChainOwnership ownership =
        qsnapper::security::accumulateAncestorChainOwnership(observed);

    QCOMPARE(qsnapper::security::chainAllowsMutation(ownership), allowed);
}

/**
 * @brief 共有された親の下で、rename-asideとrollbackが名前を動かさないことを検証する
 *
 * 移動元か移動先の親が他の利用者と共有されていれば、rootでもEPERMで拒否する
 */
void TestFilesystemHelpers::renameBeneathRootRefusesSharedParents_data()
{
    QTest::addColumn<uint>("sourceGroup");
    QTest::addColumn<uint>("sourceMode");
    QTest::addColumn<uint>("destinationGroup");
    QTest::addColumn<uint>("destinationMode");
    QTest::addColumn<bool>("expectRenamed");

    // 両方の親が所有者専用なら、所有者は名前を自由に動かせる
    QTest::newRow("rename-safe") << uint(DIRECTORY_OWNER_GID) << 0755u << uint(DIRECTORY_OWNER_GID) << 0755u << true;
    // 移動元の親が共有グループなら、他の利用者が名前を差し替えられるため拒否する
    QTest::newRow("rename-source-shared")
        << uint(SHARED_GROUP_GID) << 0770u << uint(DIRECTORY_OWNER_GID) << 0755u << false;
    // 移動先の親が共有グループなら、移動後の名前を他の利用者が差し替えられるため拒否する
    QTest::newRow("rename-destination-shared")
        << uint(DIRECTORY_OWNER_GID) << 0755u << uint(SHARED_GROUP_GID) << 0770u << false;
}

/**
 * @brief snapshotのrename-asideとrollbackが、共有された親で名前を動かさないことを検証する
 *
 * 移動元と移動先の親を所有者1000で作り、グループの書込みを他の利用者と共有する親では
 * rootでも名前を動かせないことを確かめる
 * @note 親の所有者を変えるためrootで実行し、root以外ではQSKIPする
 */
void TestFilesystemHelpers::renameBeneathRootRefusesSharedParents()
{
    if (::geteuid() != 0) {
        QSKIP("changing directory owners requires root");
    }
    QFETCH(uint, sourceGroup);
    QFETCH(uint, sourceMode);
    QFETCH(uint, destinationGroup);
    QFETCH(uint, destinationMode);
    QFETCH(bool, expectRenamed);

    qsnapper::security::detail::setUserPrivateGroupCheckForTesting(onlyOwnerGroupIsPrivate);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourceParent = root.filePath(QStringLiteral("source"));
    const QString destinationParent = root.filePath(QStringLiteral("destination"));
    QVERIFY(makeOwnedDirectory(sourceParent, DIRECTORY_OWNER_UID, sourceGroup, sourceMode));
    QVERIFY(makeOwnedDirectory(destinationParent, DIRECTORY_OWNER_UID, destinationGroup, destinationMode));
    const QString sourcePath = sourceParent + QStringLiteral("/entry");
    const QString destinationPath = destinationParent + QStringLiteral("/entry");
    QVERIFY(makeOwnedDirectory(sourcePath, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));

    errno = 0;
    const bool renamed = qsnapper::security::safeRenamePathNoFollowBeneathRoot(root.path(), sourcePath,
                                                                               destinationPath);
    const int renameError = errno;

    QCOMPARE(renamed, expectRenamed);
    QCOMPARE(QFileInfo::exists(destinationPath), expectRenamed);
    QCOMPARE(QFileInfo::exists(sourcePath), !expectRenamed);
    if (!expectRenamed) {
        // 共有された親では、rootでも名前の移動をEPERMで拒否する
        QCOMPARE(renameError, EPERM);
    }
}
