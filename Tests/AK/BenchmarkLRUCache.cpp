/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/LRUCache.h>
#include <AK/Random.h>
#include <LibMain/Main.h>
#include <math.h>
#include <stdio.h>

static constexpr size_t num_operations = 1'000'000;

struct Struct8 {
    int value {};
    u8 padding[4] {};
    bool operator==(Struct8 const&) const = default;
};

struct Struct16 {
    int value {};
    u8 padding[12] {};
    bool operator==(Struct16 const&) const = default;
};

template<>
struct AK::Traits<Struct8> : AK::DefaultTraits<Struct8> {
    static unsigned hash(Struct8 const& s) { return int_hash(s.value); }
};

template<>
struct AK::Traits<Struct16> : AK::DefaultTraits<Struct16> {
    static unsigned hash(Struct16 const& s) { return int_hash(s.value); }
};

static int volatile miss_cost_sink;

static void simulate_miss_cost(size_t iterations)
{
    int x = 42;
    for (size_t i = 0; i < iterations; ++i)
        x = x * 2654435761u + 1;
    miss_cost_sink = x;
}

static u32 zipfian_key(u32 random_value, size_t key_space, double hotness)
{
    auto normalized = static_cast<double>(random_value) / static_cast<double>(NumericLimits<u32>::max());
    return static_cast<u32>(pow(normalized, hotness) * key_space);
}

template<typename K, typename V>
static size_t bench_lru_cache(size_t capacity, size_t key_space, double hotness, size_t miss_cost,
    auto make_key, auto make_value)
{
    LRUCache<K, V> cache(capacity);
    size_t hits = 0;

    for (size_t i = 0; i < num_operations; ++i) {
        u32 raw_key = zipfian_key(get_random_uniform(NumericLimits<u32>::max()), key_space, hotness);
        auto key = make_key(raw_key);
        bool is_miss = false;
        cache.ensure(key, [&] {
            is_miss = true;
            simulate_miss_cost(miss_cost);
            return make_value(static_cast<int>(i));
        });
        if (!is_miss)
            ++hits;
    }

    return hits;
}

template<typename K, typename V>
static size_t bench_ordered_hash_map(size_t capacity, size_t key_space, double hotness, size_t miss_cost,
    auto make_key, auto make_value)
{
    OrderedHashMap<K, V> map;
    size_t hits = 0;

    for (size_t i = 0; i < num_operations; ++i) {
        u32 raw_key = zipfian_key(get_random_uniform(NumericLimits<u32>::max()), key_space, hotness);
        auto key = make_key(raw_key);
        auto it = map.find(key);
        if (it != map.end()) {
            auto value = it->value;
            map.remove(it);
            map.set(key, value);
            ++hits;
        } else {
            simulate_miss_cost(miss_cost);
            if (map.size() >= capacity)
                map.take_first();
            map.set(key, make_value(static_cast<int>(i)));
        }
    }

    return hits;
}

template<typename K, typename V>
static size_t effective_lru_capacity(size_t requested)
{
    LRUCache<K, V> temp(requested);
    return temp.maximum_size();
}

struct BenchResult {
    size_t hits;
    size_t capacity;
};

template<typename K, typename V>
static BenchResult run_for_mode(StringView mode, size_t capacity, size_t key_space, double hotness, size_t miss_cost,
    auto make_key, auto make_value)
{
    auto effective_capacity = effective_lru_capacity<K, V>(capacity);
    size_t hits;
    if (mode == "lru"sv)
        hits = bench_lru_cache<K, V>(effective_capacity, key_space, hotness, miss_cost, make_key, make_value);
    else
        hits = bench_ordered_hash_map<K, V>(effective_capacity, key_space, hotness, miss_cost, make_key, make_value);
    return { hits, effective_capacity };
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView mode;
    StringView type;
    size_t capacity = 4096;
    double hotness = 2.0;
    size_t miss_cost = 0;

    for (size_t i = 1; i < arguments.strings.size(); ++i) {
        auto arg = arguments.strings[i];
        if (arg.starts_with("--mode="sv))
            mode = arg.substring_view(7);
        else if (arg.starts_with("--type="sv))
            type = arg.substring_view(7);
        else if (arg.starts_with("--size="sv))
            capacity = arg.substring_view(7).to_number<size_t>().value_or(4096);
        else if (arg.starts_with("--hotness="sv))
            hotness = arg.substring_view(10).to_number<double>().value_or(2.0);
        else if (arg.starts_with("--miss-cost="sv))
            miss_cost = arg.substring_view(12).to_number<size_t>().value_or(0);
    }

    if (mode.is_empty() || type.is_empty()) {
        fprintf(stderr, "Usage: BenchmarkLRUCache --mode=lru|ordered --type=int|s16|s8s8"
                        " [--size=N] [--hotness=H] [--miss-cost=N]\n");
        return 1;
    }

    size_t key_space = capacity * 4;
    auto concentration = pow(0.5, hotness) * 100.0;

    BenchResult result {};
    char const* type_description = "";
    if (type == "int"sv) {
        type_description = "<u32, int>";
        result = run_for_mode<u32, int>(mode, capacity, key_space, hotness, miss_cost, [](u32 k) -> u32 { return k; }, [](int v) -> int { return v; });
    } else if (type == "s16"sv) {
        type_description = "<u32, Struct16>";
        result = run_for_mode<u32, Struct16>(mode, capacity, key_space, hotness, miss_cost, [](u32 k) -> u32 { return k; }, [](int v) -> Struct16 { return { v }; });
    } else if (type == "s8s8"sv) {
        type_description = "<Struct8, Struct8>";
        result = run_for_mode<Struct8, Struct8>(mode, capacity, key_space, hotness, miss_cost, [](u32 k) -> Struct8 { return { static_cast<int>(k) }; }, [](int v) -> Struct8 { return { v }; });
    } else {
        fprintf(stderr, "Unknown type: %.*s\n",
            static_cast<int>(type.length()), type.characters_without_null_termination());
        return 1;
    }

    auto hit_rate = static_cast<double>(result.hits) / num_operations * 100.0;
    fprintf(stderr, "Mode: %.*s | Type: %s | Capacity: %zu | Hotness: %.1f (50%% hit %.1f%% of keys) "
                    "| Miss cost: %zu | Hits: %zu | Hit rate: %.1f%%\n",
        static_cast<int>(mode.length()), mode.characters_without_null_termination(),
        type_description, result.capacity, hotness, concentration, miss_cost, result.hits, hit_rate);

    return 0;
}
