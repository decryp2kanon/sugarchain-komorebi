// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Synthetic cache-policy diagnostic, NOT a PoW or full-IBD benchmark.
// Uses the production cache type but never inserts synthetic evidence into a node.
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <cuckoocache.h>
#include <uint256.h>
#include <util/hasher.h>

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

static uint64_t Number(std::string_view s, uint64_t max)
{
    uint64_t n{};
    const auto [end, error]{std::from_chars(s.data(), s.data() + s.size(), n)};
    if (error != std::errc{} || end != s.data() + s.size() || !n || n > max) throw std::runtime_error("Invalid argument");
    return n;
}

static uint256 Entry(uint64_t index)
{
    unsigned char input[8];
    WriteLE64(input, index);
    uint256 result;
    CSHA256{}.Write(input, sizeof(input)).Finalize(result.begin());
    return result;
}

int main(int argc, char** argv)
{
    try {
        if (argc != 4) throw std::runtime_error("Usage: pow-cache-retention CACHE_MIB HEADERS SAMPLE_STRIDE");
        const auto bytes{Number(argv[1], 2048) << 20};
        const auto headers{Number(argv[2], 100'000'000)};
        const auto stride{Number(argv[3], headers)};
        CuckooCache::cache<uint256, SignatureCacheHasher> cache;
        const auto [capacity, storage]{cache.setup_bytes(bytes)};
        for (uint64_t i{0}; i < headers; ++i) cache.insert(Entry(i));
        std::cout << "capacity,storage_bytes,headers,bucket,sampled,hits\n";
        // Read-only chronological deciles: do not refresh/insert misses, which
        // would change the state we are trying to measure.
        for (uint64_t bucket{0}; bucket < 10; ++bucket) {
            uint64_t samples{0}, hits{0};
            for (uint64_t i{headers * bucket / 10}; i < headers * (bucket + 1) / 10; i += stride) {
                ++samples;
                hits += cache.contains(Entry(i), false);
            }
            std::cout << capacity << ',' << storage << ',' << headers << ',' << bucket << ',' << samples << ',' << hits << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
