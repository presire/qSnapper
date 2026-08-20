#ifndef COMPARISONCACHE_H
#define COMPARISONCACHE_H

#include <memory>
#include <optional>
#include <QString>

/**
 * @brief Single-entry, non-thread-safe cache for a libsnapper Comparison.
 *
 * Stores at most one T, keyed by a directional (config, first, optional second)
 * snapshot pair. Designed for the qSnapper D-Bus service's single Qt event-loop
 * thread — not safe for concurrent use.
 *
 * The cached entry is owned via std::unique_ptr. Callers receive a raw T* that
 * remains valid only until the next cache operation (get with a different key
 * or Refresh policy, clear, or cache destruction).
 *
 * Lifetime contract:
 *   - m_snapper must be declared before the cache in the owning class so that
 *     the cache (and its Comparison) is destroyed before the Snapper.
 *   - The cache must be cleared before any Snapper replacement or
 *     snapshot/current-system mutation.
 */
template <typename T>
class ComparisonCache
{
public:
    /**
     * @brief Directional cache key.
     *
     * @var config  Normalized Snapper config name.
     * @var first   First (source) snapshot number.
     * @var second  Second (target) snapshot number; nullopt denotes
     *              a single-vs-current comparison.
     *
     * (config, 1, 2) and (config, 2, 1) are distinct keys.
     * (config, 1, nullopt) and (config, 1, 2) are distinct keys.
     */
    struct Key {
        QString config;
        int first;
        std::optional<int> second;

        bool operator==(const Key &other) const
        {
            return config == other.config
                && first == other.first
                && second == other.second;
        }
    };

    /**
     * @brief Cache access policy.
     *
     * - Reuse:   Return the cached entry when the key matches; create only on miss.
     * - Refresh: Always create a fresh entry, replacing any existing one.
     */
    enum class Policy {
        Reuse,
        Refresh
    };

    /**
     * @brief Return a cached or freshly-created entry.
     *
     * @param key     Directional key.
     * @param policy  Reuse or Refresh.
     * @param factory Callable invoked on cache miss / refresh, returning
     *               std::unique_ptr<T>. Not invoked on Reuse hit.
     * @return Pointer to the entry (never null if factory returned non-null).
     *
     * Exception safety: if factory throws, the cache is left empty —
     * the old entry is destroyed before the factory is called, so no stale
     * entry survives a failed refresh.
     */
    template <typename Factory>
    T *get(const Key &key, Policy policy, Factory &&factory)
    {
        if (policy == Policy::Reuse && m_key && *m_key == key && m_entry) {
            return m_entry.get();
        }
        // Clear first so a factory exception leaves the cache empty.
        m_entry.reset();
        m_key.reset();
        m_entry = factory(key);
        m_key = key;
        return m_entry.get();
    }

    /**
     * @brief Whether the cache currently holds an entry matching key.
     */
    bool hasKey(const Key &key) const
    {
        return m_key && *m_key == key && m_entry;
    }

    /**
     * @brief Invalidate the cache entry.
     *
     * Destroys the stored T (which, for snapper::Comparison, unmounts snapshots).
     */
    void clear()
    {
        m_entry.reset();
        m_key.reset();
    }

private:
    std::optional<Key> m_key;
    std::unique_ptr<T> m_entry;
};

#endif // COMPARISONCACHE_H
