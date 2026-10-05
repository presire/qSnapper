/**
 * @file comparisoncache.h
 * @brief libsnapper比較結果の単一エントリ保持キャッシュ
 *
 * D-Busサービスの単一Qtイベントループスレッドから使う非スレッドセーフな保持器である
 * 保持できるのは高々1件であり、方向付きのスナップショット対を鍵とする
 */

#ifndef QSNAPPER_COMPARISONCACHE_H
#define QSNAPPER_COMPARISONCACHE_H

#include <memory>
#include <optional>
#include <QString>

/**
 * @class ComparisonCache
 * @brief libsnapper比較結果を高々1件だけ保持するキャッシュ
 *
 * 方向付きの (設定名、先頭、任意の後続) スナップショット対を鍵とする
 * qSnapper D-Busサービスの単一Qtイベントループスレッド専用であり、並行利用は安全でない
 *
 * 保持した要素は std::unique_ptr で所有し、呼び出し側へ渡す生ポインタは
 * 次のcache操作 (別鍵でのget、Refresh指定のget、clear、cache破棄) まで有効である
 *
 * 存続契約:
 *   - 所有側クラスでは m_snapper より後にcacheを宣言すること
 *     破棄順により cache (と比較結果) が Snapper より先に破棄される
 *   - Snapperの置き換えやスナップショットと現システムの変更の前にcacheをclearすること
 *
 * m_key : 現在保持している要素の方向付き鍵 (空なら未保持)
 * m_entry : 現在保持している要素の所有権
 */
template <typename T>
class ComparisonCache
{
public:
    /**
     * @class Key
     * @brief 比較結果の方向付きcache鍵
     *
     * config : 正規化済みSnapper設定名
     * first : 先頭 (比較元) スナップショット番号
     * second : 後続 (比較先) スナップショット番号 (空なら現システムとの単一比較)
     *
     * (設定、1、2) と (設定、2、1) は別鍵として扱う
     * (設定、1、空) と (設定、1、2) は別鍵として扱う
     */
    struct Key {
        QString config;              // 正規化済みSnapper設定名
        int first;                   // 先頭 (比較元) スナップショット番号
        std::optional<int> second;   // 後続 (比較先) 番号 (空なら現システムとの比較)

        bool operator==(const Key &other) const
        {
            return config == other.config
                && first == other.first
                && second == other.second;
        }
    };

    /**
     * @class Policy
     * @brief cache参照の方針
     *
     * Reuse : 鍵一致で保持要素を返し、miss時のみ生成する
     * Refresh : 常に新規生成し、既存要素を置き換える
     */
    enum class Policy {
        Reuse,   // 鍵一致で保持要素を返す
        Refresh  // 常に新規生成して置き換える
    };

    /**
     * @brief 保持要素または新規生成要素を返す
     *
     * @param key 方向付き鍵
     * @param policy ReuseまたはRefresh
     * @param factory miss時とrefresh時にだけ呼ぶ生成器 (std::unique_ptr<T> を返す)
     *                Reuseのhit時は呼ばない
     * @return 要素へのポインタ (生成器が非空を返した場合は非null)
     *
     * 例外安全性: 生成器が例外を投げた場合cacheは空のまま残る
     * 生成前に古い要素を破棄するため、失敗したrefreshの古い要素が残らない
     */
    template <typename Factory>
    T *get(const Key &key, Policy policy, Factory &&factory)
    {
        if (policy == Policy::Reuse && m_key && *m_key == key && m_entry) {
            return m_entry.get();
        }
        // 先に破棄するため、生成器の例外時はcacheが空になる
        m_entry.reset();
        m_key.reset();
        m_entry = factory(key);
        m_key = key;
        return m_entry.get();
    }

    /**
     * @brief 鍵に一致する要素を保持しているか返す
     * @param key 照合する方向付き鍵
     * @return 鍵一致の要素を保持していればtrue
     */
    bool hasKey(const Key &key) const
    {
        return m_key && *m_key == key && m_entry;
    }

    /**
     * @brief cache要素を無効化する
     *
     * 保持したTを破棄する (snapper比較結果の場合はスナップショットをunmountする)
     */
    void clear()
    {
        m_entry.reset();
        m_key.reset();
    }

private:
    std::optional<Key> m_key;   // 現在保持している要素の方向付き鍵 (空なら未保持)
    std::unique_ptr<T> m_entry; // 現在保持している要素の所有権
};

#endif // QSNAPPER_COMPARISONCACHE_H
