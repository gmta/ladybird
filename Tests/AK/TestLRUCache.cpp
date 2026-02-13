/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/LRUCache.h>

TEST_CASE(basic_insert_and_get)
{
    LRUCache<int, int> cache(4);
    cache.set(1, 10);
    cache.set(2, 20);
    cache.set(3, 30);

    EXPECT_EQ(*cache.get(1), 10);
    EXPECT_EQ(*cache.get(2), 20);
    EXPECT_EQ(*cache.get(3), 30);
    EXPECT_EQ(cache.get(99), nullptr);
}

TEST_CASE(eviction_at_capacity)
{
    // With 1 set and 5 ways, eviction is exact LRU within the set.
    LRUCache<int, int> cache(5);
    cache.set(1, 10);
    cache.set(2, 20);
    cache.set(3, 30);
    cache.set(4, 40);
    cache.set(5, 50);

    // Insert a sixth; key 1 (oldest) should be evicted.
    cache.set(6, 60);
    EXPECT_EQ(cache.get(1), nullptr);
    EXPECT_EQ(*cache.get(6), 60);
}

TEST_CASE(access_promotes_entry)
{
    LRUCache<int, int> cache(5);
    cache.set(1, 10);
    cache.set(2, 20);
    cache.set(3, 30);
    cache.set(4, 40);
    cache.set(5, 50);

    // Access key 1 to promote it.
    cache.get(1);

    // Insert key 6; key 2 (oldest un-accessed) should be evicted instead of key 1.
    cache.set(6, 60);
    EXPECT(cache.contains(1));
    EXPECT(!cache.contains(2));
    EXPECT(cache.contains(3));
    EXPECT(cache.contains(6));
}

TEST_CASE(remove)
{
    LRUCache<int, int> cache(4);
    cache.set(1, 10);
    cache.set(2, 20);

    EXPECT(cache.remove(1));
    EXPECT_EQ(cache.get(1), nullptr);

    EXPECT(!cache.remove(99));
}

TEST_CASE(set_updates_existing)
{
    LRUCache<int, int> cache(4);
    cache.set(1, 10);
    cache.set(1, 99);

    EXPECT_EQ(*cache.get(1), 99);
}

TEST_CASE(contains)
{
    LRUCache<int, int> cache(4);
    cache.set(1, 10);

    EXPECT(cache.contains(1));
    EXPECT(!cache.contains(2));
}

TEST_CASE(maximum_size)
{
    // 42 / 5 ways = 8 sets (already a power of 2)
    LRUCache<int, int> cache(42);
    EXPECT_EQ(cache.maximum_size(), 40u);
}

TEST_CASE(ensure_creates_entry)
{
    LRUCache<int, int> cache(4);
    auto& value = cache.ensure(1, [] { return 42; });
    EXPECT_EQ(value, 42);
}

TEST_CASE(ensure_returns_existing)
{
    LRUCache<int, int> cache(4);
    cache.set(1, 10);
    auto& value = cache.ensure(1, [] { return 99; });
    EXPECT_EQ(value, 10);
}
