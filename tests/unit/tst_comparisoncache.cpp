/**
 * @file tst_comparisoncache.cpp
 * @brief ComparisonCache<T> の単体テスト
 *
 * Fake型とFakeファクトリを用いて、キャッシュ契約 (再利用・リフレッシュ・
 * キー分離・方向性・クリア・例外安全性) を検証する
 */

#include <QtTest/QtTest>
#include <QString>
#include <memory>
#include <optional>
#include <stdexcept>

#include "comparisoncache.h"

namespace {

/**
 * @struct FakeComparison
 * @brief 生成回数と破棄回数を静的カウンタで追跡するテスト用型
 *
 * キャッシュがエントリを再利用したか新規構築したか、破棄したかを
 * カウンタの増減で判定するために使う
 */
struct FakeComparison {
    static int s_constructCount;  // 生成回数の累計
    static int s_destroyCount;    // 破棄回数の累計
    /** @brief 生成回数と破棄回数のカウンタを0に戻す */
    static void resetCounters()
    {
        s_constructCount = 0;
        s_destroyCount = 0;
    }

    int id;  // インスタンス識別子

    /**
     * @brief 識別子を指定して生成し生成カウンタを増やす
     * @param id_ このインスタンスの識別子
     */
    explicit FakeComparison(int id_) : id(id_)
    {
        ++s_constructCount;
    }

    /** @brief 破棄時に破棄カウンタを増やす */
    ~FakeComparison()
    {
        ++s_destroyCount;
    }

    /** @brief コピー構築を禁止する */
    FakeComparison(const FakeComparison &) = delete;
    /** @brief コピー代入を禁止する */
    FakeComparison &operator=(const FakeComparison &) = delete;
};

int FakeComparison::s_constructCount = 0;
int FakeComparison::s_destroyCount = 0;

} // namespace

/**
 * @class TestComparisonCache
 * @brief ComparisonCache<T> のキャッシュ契約を確認するテストスイート
 *
 * 再利用・リフレッシュ・キー分離・方向性・クリア・例外安全性の各契約を
 * FakeComparison のカウンタで確認する
 */
class TestComparisonCache : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 同一キーで2回目はファクトリを呼ばず再利用することを確認する */
    void reuseSkipsFactoryOnSecondCall();

    /** @brief 異なるキーだとファクトリを呼び直すことを確認する */
    void reuseRebuildsOnKeyMismatch();

    /** @brief 同一キーでもRefreshは必ず新規構築することを確認する */
    void refreshAlwaysRebuilds();

    /** @brief Refreshでファクトリ例外時にキャッシュが空になることを確認する */
    void refreshLeavesEmptyOnFactoryThrow();

    /** @brief Reuseでファクトリ例外時にキャッシュが空になることを確認する */
    void reuseLeavesEmptyOnFactoryThrow();

    /** @brief キーのconfig次元が区別されることを確認する */
    void keyDistinguishesConfig();
    /** @brief キーのfirst次元が区別されることを確認する */
    void keyDistinguishesFirst();
    /** @brief キーのsecond次元が区別されることを確認する */
    void keyDistinguishesSecond();
    /** @brief single (second=nullopt) とbetween (secondあり) が区別されることを確認する */
    void keyDistinguishesSingleVsBetween();

    /** @brief キーが方向性を持ち (config,1,2) と (config,2,1) が別キーになることを確認する */
    void keyIsDirectional();

    /** @brief hasKey() がキャッシュの状態を正しく反映することを確認する */

    /** @brief clear() がエントリを破棄することを確認する */
    void clearDestroysEntry();

    /** @brief デストラクタがキャッシュ破棄時にエントリも破棄することを確認する */
    void destructionReleasesEntry();
};

/**
 * @brief 同一キーで2回目はファクトリを呼ばず再利用することを確認する
 */
void TestComparisonCache::reuseSkipsFactoryOnSecondCall()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(100));
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};

    FakeComparison *p1 = cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(p1 != nullptr);
    QCOMPARE(p1->id, 100);
    QCOMPARE(FakeComparison::s_constructCount, 1);

    // 2回目はファクトリを呼ばず再利用
    FakeComparison *p2 = cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(p2 == p1);
    QCOMPARE(FakeComparison::s_constructCount, 1);
}

/**
 * @brief 異なるキーだとファクトリを呼び直すことを確認する
 */
void TestComparisonCache::reuseRebuildsOnKeyMismatch()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(200));
    };

    ComparisonCache<FakeComparison>::Key key1{QStringLiteral("root"), 1, std::nullopt};
    ComparisonCache<FakeComparison>::Key key2{QStringLiteral("root"), 2, std::nullopt};

    cache.get(key1, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QCOMPARE(FakeComparison::s_constructCount, 1);

    // 異なるキー → 新規構築
    FakeComparison *p2 = cache.get(key2, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(p2 != nullptr);
    QCOMPARE(p2->id, 200);
    QCOMPARE(FakeComparison::s_constructCount, 2);
    QVERIFY(FakeComparison::s_destroyCount >= 1);
}

/**
 * @brief 同一キーでもRefreshは必ず新規構築することを確認する
 */
void TestComparisonCache::refreshAlwaysRebuilds()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(300));
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};

    cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QCOMPARE(FakeComparison::s_constructCount, 1);

    // Refresh: 同一キーでも新規構築
    FakeComparison *p2 = cache.get(key, ComparisonCache<FakeComparison>::Policy::Refresh, factory);
    QVERIFY(p2 != nullptr);
    QCOMPARE(FakeComparison::s_constructCount, 2);
    QVERIFY(FakeComparison::s_destroyCount >= 1);
}

/**
 * @brief Refreshでファクトリ例外時にキャッシュが空になることを確認する
 *
 * 古いエントリは破棄済みであり、例外後にhasKey()がfalseになることを確認する
 */
void TestComparisonCache::refreshLeavesEmptyOnFactoryThrow()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(400));
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};
    cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QCOMPARE(FakeComparison::s_constructCount, 1);

    // ファクトリが例外を投げるRefresh
    auto throwingFactory = [](const ComparisonCache<FakeComparison>::Key &) -> std::unique_ptr<FakeComparison> {
        throw std::runtime_error("factory failure");
    };

    bool threw = false;
    try {
        cache.get(key, ComparisonCache<FakeComparison>::Policy::Refresh, throwingFactory);
    }
    catch (const std::runtime_error &) {
        threw = true;
    }
    QVERIFY(threw);

    // 例外後はキャッシュが空 (古いエントリは破棄済み)
    QVERIFY(!cache.hasKey(key));
    // キャッシュが空なのでhasKeyはfalse、ただし破棄されたことを確認
    QCOMPARE(FakeComparison::s_destroyCount, 1);
}

/**
 * @brief Reuseでファクトリ例外時にキャッシュが空になることを確認する
 *
 * 最初は空なのでReuseでもファクトリが呼ばれ、例外後にhasKey()がfalseになることを確認する
 */
void TestComparisonCache::reuseLeavesEmptyOnFactoryThrow()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    // 最初は空なのでReuseでもファクトリが呼ばれる
    auto throwingFactory = [](const ComparisonCache<FakeComparison>::Key &) -> std::unique_ptr<FakeComparison> {
        throw std::runtime_error("factory failure");
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};

    bool threw = false;
    try {
        cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, throwingFactory);
    }
    catch (const std::runtime_error &) {
        threw = true;
    }
    QVERIFY(threw);

    // 例外後はキャッシュが空
    QVERIFY(!cache.hasKey(key));
}

/**
 * @brief キーのconfig次元が区別されることを確認する
 */
void TestComparisonCache::keyDistinguishesConfig()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(500));
    };

    ComparisonCache<FakeComparison>::Key key1{QStringLiteral("root"), 1, std::nullopt};
    ComparisonCache<FakeComparison>::Key key2{QStringLiteral("home"), 1, std::nullopt};

    cache.get(key1, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(key1));
    QVERIFY(!cache.hasKey(key2));

    cache.get(key2, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(key2));
    QVERIFY(!cache.hasKey(key1));
    QCOMPARE(FakeComparison::s_constructCount, 2);
}

/**
 * @brief キーのfirst次元が区別されることを確認する
 */
void TestComparisonCache::keyDistinguishesFirst()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(600));
    };

    ComparisonCache<FakeComparison>::Key key1{QStringLiteral("root"), 1, std::nullopt};
    ComparisonCache<FakeComparison>::Key key2{QStringLiteral("root"), 2, std::nullopt};

    cache.get(key1, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(key1));
    QVERIFY(!cache.hasKey(key2));

    cache.get(key2, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(!cache.hasKey(key1));
    QVERIFY(cache.hasKey(key2));
}

/**
 * @brief キーのsecond次元が区別されることを確認する
 */
void TestComparisonCache::keyDistinguishesSecond()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(700));
    };

    ComparisonCache<FakeComparison>::Key key1{QStringLiteral("root"), 1, 2};
    ComparisonCache<FakeComparison>::Key key2{QStringLiteral("root"), 1, 3};

    cache.get(key1, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(key1));
    QVERIFY(!cache.hasKey(key2));

    cache.get(key2, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(!cache.hasKey(key1));
    QVERIFY(cache.hasKey(key2));
}

/**
 * @brief single (second=nullopt) とbetween (secondあり) が区別されることを確認する
 */
void TestComparisonCache::keyDistinguishesSingleVsBetween()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(800));
    };

    // (root, 1, nullopt) は single/current
    // (root, 1, 2) は between
    ComparisonCache<FakeComparison>::Key singleKey{QStringLiteral("root"), 1, std::nullopt};
    ComparisonCache<FakeComparison>::Key betweenKey{QStringLiteral("root"), 1, 2};

    cache.get(singleKey, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(singleKey));
    QVERIFY(!cache.hasKey(betweenKey));

    cache.get(betweenKey, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(!cache.hasKey(singleKey));
    QVERIFY(cache.hasKey(betweenKey));
    QCOMPARE(FakeComparison::s_constructCount, 2);
}

/**
 * @brief キーが方向性を持ち (config,1,2) と (config,2,1) が別キーになることを確認する
 */
void TestComparisonCache::keyIsDirectional()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(900));
    };

    ComparisonCache<FakeComparison>::Key key12{QStringLiteral("root"), 1, 2};
    ComparisonCache<FakeComparison>::Key key21{QStringLiteral("root"), 2, 1};

    cache.get(key12, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(cache.hasKey(key12));
    QVERIFY(!cache.hasKey(key21));

    cache.get(key21, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QVERIFY(!cache.hasKey(key12));
    QVERIFY(cache.hasKey(key21));
    QCOMPARE(FakeComparison::s_constructCount, 2);
}

/**
 * @brief clear() がエントリを破棄することを確認する
 *
 * クリア後にReuseで再構築できることも確認する
 */
void TestComparisonCache::clearDestroysEntry()
{
    FakeComparison::resetCounters();
    ComparisonCache<FakeComparison> cache;

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(1100));
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};

    cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QCOMPARE(FakeComparison::s_constructCount, 1);
    QCOMPARE(FakeComparison::s_destroyCount, 0);

    cache.clear();
    QCOMPARE(FakeComparison::s_destroyCount, 1);
    QVERIFY(!cache.hasKey(key));

    // クリア後のReuseで再構築可能
    cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
    QCOMPARE(FakeComparison::s_constructCount, 2);
}

/**
 * @brief デストラクタがキャッシュ破棄時にエントリも破棄することを確認する
 */
void TestComparisonCache::destructionReleasesEntry()
{
    FakeComparison::resetCounters();

    auto factory = [](const ComparisonCache<FakeComparison>::Key &) {
        return std::unique_ptr<FakeComparison>(new FakeComparison(1200));
    };

    ComparisonCache<FakeComparison>::Key key{QStringLiteral("root"), 1, std::nullopt};

    {
        ComparisonCache<FakeComparison> cache;
        cache.get(key, ComparisonCache<FakeComparison>::Policy::Reuse, factory);
        QCOMPARE(FakeComparison::s_constructCount, 1);
        QCOMPARE(FakeComparison::s_destroyCount, 0);
    }
    // スコープ退出でキャッシュと共にエントリも破棄
    QCOMPARE(FakeComparison::s_destroyCount, 1);
}

QTEST_APPLESS_MAIN(TestComparisonCache)
#include "tst_comparisoncache.moc"
