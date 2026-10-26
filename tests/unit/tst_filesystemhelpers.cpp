/**
 * @file tst_filesystemhelpers.cpp
 * @brief 共通fixtureとテスト実行体
 *
 * errno判定・指紋・攻撃再現fixtureの実装とQTEST実行マクロを置く
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <unistd.h>

#include "filesystemhelpers.h"

#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;
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
 * センチネル secret.txt、outside/b に decoy target.txt を配置する
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
    QVERIFY(safeRemoveAllBeneathRoot(rootDir.path(), componentPath));
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

QTEST_APPLESS_MAIN(TestFilesystemHelpers)
