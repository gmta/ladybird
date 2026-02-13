/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntegralMath.h>
#include <AK/NumericLimits.h>
#include <AK/Traits.h>
#include <AK/kmalloc.h>

namespace AK {

// Set-associative LRU cache inspired by CPU cache design. Keys are mapped to a fixed "set" via hashing, and each set
// holds `WAYS` entries. On eviction, the least-recently-used entry within the target set is replaced. This gives O(1)
// lookups with excellent cache-line locality.
//
// Each entry stores a u32 hash for fast rejection before expensive key comparison. LRU timestamps are kept in a
// separate allocation from key/value data so that updating a timestamp on cache hits does not dirty the entries' cache
// line.
//
// The number of sets is dynamically grown from 1 up to `max_size / WAYS` (rounded to a power of two), doubling when
// occupancy exceeds 75%. This keeps memory usage proportional to actual cache utilization.
template<typename K, typename V, typename KeyTraits = Traits<K>>
class LRUCache {
    struct Entry {
        K key {};
        V value {};
        u32 stored_hash {};
    };

    static constexpr size_t CACHE_LINE_SIZE = 64;
    static constexpr size_t WAYS = CACHE_LINE_SIZE / sizeof(Entry);
    static constexpr u64 EMPTY_SENTINEL = 0;

    // WAYS=1 degrades to direct-mapped caching where the hit rate loss from conflict misses outweighs the speed gain
    // over alternatives like OrderedHashMap, so we require at least 2 entries per cache line.
    static_assert(WAYS >= 2, "Key/value types are too large for effective set-associative caching");

    struct CacheSet {
        Entry entries[WAYS];
    };

public:
    explicit LRUCache(size_t max_size)
        : m_max_number_of_sets(1uz << ceil_log2(max(max_size / WAYS, 1uz)))
        , m_set_mask(min(m_max_number_of_sets, 4uz) - 1)
        , m_sets(static_cast<CacheSet*>(kmalloc(allocation_size_bytes_for_sets(number_of_sets()))))
        , m_timestamps(static_cast<u64*>(kcalloc(1, allocation_size_bytes_for_timestamps(number_of_sets()))))
    {
        VERIFY(max_size > 0);
    }

    ~LRUCache()
    {
        kfree_sized(m_sets, allocation_size_bytes_for_sets(number_of_sets()));
        kfree_sized(m_timestamps, allocation_size_bytes_for_timestamps(number_of_sets()));
    }

    LRUCache(LRUCache const&) = delete;
    LRUCache& operator=(LRUCache const&) = delete;

    V* get(K const& key)
    {
        auto hash = KeyTraits::hash(key);
        auto* entries = m_sets[hash & m_set_mask].entries;
        auto* ts = &m_timestamps[(hash & m_set_mask) * WAYS];
        for (size_t i = 0; i < WAYS; ++i) {
            if (entries[i].stored_hash == hash && entries[i].key == key && ts[i] != EMPTY_SENTINEL) {
                ts[i] = ++m_clock;
                return &entries[i].value;
            }
        }

        return nullptr;
    }

    void set(K key, V value)
    {
        if (should_grow())
            grow();

        auto hash = KeyTraits::hash(key);
        auto set_index = hash & m_set_mask;
        auto* entries = m_sets[set_index].entries;
        auto* ts = &m_timestamps[set_index * WAYS];
        auto victim = find_or_select_victim(entries, ts, key, hash);

        if (victim.is_existing_match) {
            entries[victim.index].value = move(value);
            ts[victim.index] = ++m_clock;
            return;
        }

        if (ts[victim.index] == EMPTY_SENTINEL)
            ++m_occupied;

        ts[victim.index] = ++m_clock;
        entries[victim.index] = { move(key), move(value), hash };
    }

    template<typename Callback>
    V& ensure(K const& key, Callback initialization_callback) { return ensure_impl(key, move(initialization_callback)); }

    template<typename Callback>
    V& ensure(K&& key, Callback initialization_callback) { return ensure_impl(move(key), move(initialization_callback)); }

    bool remove(K const& key)
    {
        auto hash = KeyTraits::hash(key);
        auto set_index = hash & m_set_mask;
        auto* entries = m_sets[set_index].entries;
        auto* ts = &m_timestamps[set_index * WAYS];
        for (size_t i = 0; i < WAYS; ++i) {
            if (entries[i].stored_hash == hash && entries[i].key == key && ts[i] != EMPTY_SENTINEL) {
                ts[i] = EMPTY_SENTINEL;
                entries[i] = {};
                --m_occupied;
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] bool contains(K const& key) const
    {
        auto hash = KeyTraits::hash(key);
        auto const* entries = m_sets[hash & m_set_mask].entries;
        auto const* ts = &m_timestamps[(hash & m_set_mask) * WAYS];
        for (size_t i = 0; i < WAYS; ++i) {
            if (entries[i].stored_hash == hash && entries[i].key == key && ts[i] != EMPTY_SENTINEL)
                return true;
        }

        return false;
    }

    [[nodiscard]] constexpr size_t maximum_size() const { return m_max_number_of_sets * WAYS; }

private:
    struct VictimResult {
        size_t index;
        bool is_existing_match;
    };

    // Scan the set for an existing match on `key`. If found, return its index. Otherwise, return the best victim
    // slot: prefer empty slots, then the entry with the smallest (oldest) timestamp.
    VictimResult find_or_select_victim(Entry const* entries, u64 const* ts, K const& key, u32 hash) const
    {
        // Phase 1: scan entries for a match. The hash and key checks stay on the entries cache line; the sentinel
        // check is last so it only touches the timestamps cache line on the rare hash+key false positive.
        for (size_t i = 0; i < WAYS; ++i) {
            if (entries[i].stored_hash == hash && entries[i].key == key && ts[i] != EMPTY_SENTINEL)
                return { i, true };
        }

        // Phase 2: no match — find victim (touches timestamps cache line).
        size_t victim = 0;
        u64 min_timestamp = NumericLimits<u64>::max();
        for (size_t i = 0; i < WAYS; ++i) {
            if (ts[i] == EMPTY_SENTINEL)
                return { i, false };
            if (ts[i] < min_timestamp) {
                min_timestamp = ts[i];
                victim = i;
            }
        }

        return { victim, false };
    }

    template<typename KFwd, typename Callback>
    V& ensure_impl(KFwd&& key, Callback initialization_callback)
    {
        if (should_grow())
            grow();

        auto hash = KeyTraits::hash(key);
        auto set_index = hash & m_set_mask;
        auto* entries = m_sets[set_index].entries;
        auto* ts = &m_timestamps[set_index * WAYS];
        __builtin_prefetch(entries, 0, 3);
        __builtin_prefetch(ts, 1, 3);
        auto victim = find_or_select_victim(entries, ts, key, hash);

        if (victim.is_existing_match) {
            ts[victim.index] = ++m_clock;
            return entries[victim.index].value;
        }

        if (ts[victim.index] == EMPTY_SENTINEL)
            ++m_occupied;

        ts[victim.index] = ++m_clock;
        entries[victim.index] = { forward<KFwd>(key), initialization_callback(), hash };
        return entries[victim.index].value;
    }

    size_t number_of_sets() const { return m_set_mask + 1; }
    static size_t allocation_size_bytes_for_sets(size_t number_of_sets) { return number_of_sets * sizeof(CacheSet); }
    static size_t allocation_size_bytes_for_timestamps(size_t number_of_sets) { return number_of_sets * WAYS * sizeof(u64); }

    bool should_grow() const
    {
        return number_of_sets() < m_max_number_of_sets
            && m_occupied * 4 >= number_of_sets() * WAYS * 3;
    }

    void grow()
    {
        auto new_number_of_sets = min(number_of_sets() * 2, m_max_number_of_sets);
        auto new_mask = new_number_of_sets - 1;
        auto* new_sets = static_cast<CacheSet*>(kmalloc(allocation_size_bytes_for_sets(new_number_of_sets)));
        auto* new_ts = static_cast<u64*>(kcalloc(1, allocation_size_bytes_for_timestamps(new_number_of_sets)));

        for (size_t s = 0; s < number_of_sets(); ++s) {
            auto* old_set_ts = &m_timestamps[s * WAYS];
            for (size_t w = 0; w < WAYS; ++w) {
                if (old_set_ts[w] == EMPTY_SENTINEL)
                    continue;

                auto new_si = m_sets[s].entries[w].stored_hash & new_mask;
                auto* new_entries = new_sets[new_si].entries;
                auto* new_set_ts = &new_ts[new_si * WAYS];

                // Find an empty slot in the target set.
                bool placed = false;
                for (size_t i = 0; i < WAYS; ++i) {
                    if (new_set_ts[i] == EMPTY_SENTINEL) {
                        new_set_ts[i] = old_set_ts[w];
                        new_entries[i] = move(m_sets[s].entries[w]);
                        placed = true;
                        break;
                    }
                }
                if (placed)
                    continue;

                // No empty slot: keep whichever entry is newer between the incoming one and the set's LRU.
                size_t lru = 0;
                for (size_t i = 1; i < WAYS; ++i) {
                    if (new_set_ts[i] < new_set_ts[lru])
                        lru = i;
                }
                if (old_set_ts[w] > new_set_ts[lru]) {
                    new_set_ts[lru] = old_set_ts[w];
                    new_entries[lru] = move(m_sets[s].entries[w]);
                }
                --m_occupied;
            }
        }

        kfree_sized(m_sets, allocation_size_bytes_for_sets(number_of_sets()));
        kfree_sized(m_timestamps, allocation_size_bytes_for_timestamps(number_of_sets()));
        m_sets = new_sets;
        m_timestamps = new_ts;
        m_set_mask = new_mask;
    }

    size_t m_max_number_of_sets;
    size_t m_set_mask;
    CacheSet* m_sets;
    u64* m_timestamps;
    size_t m_occupied { 0 };
    u64 m_clock { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::LRUCache;
#endif
