// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.
// Linux/glibc diagnostics; see ../post-ibd-cache-release.md.
// Allocation fixture only. No mainnet data or admitted synthetic PoW evidence.
#include <pow.cpp> // Standalone fixture only; never link this into a node.
#include <coins.h>
#include <txdb.h>
#include <random.h>
#include <kernel/cs_main.h>
#include <script/script.h>
#include <sync.h>
#include <util/translation.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

const TranslateFn G_TRANSLATION_FUN{nullptr};
using Clock = std::chrono::steady_clock;

uint256 Key(uint64_t n)
{
    uint256 key;
    for (int i{0}; i < 8; ++i) key.data()[i] = (n >> (8 * i)) & 255;
    return key;
}

size_t Rss()
{
    std::ifstream file{"/proc/self/statm"};
    size_t virtual_pages{}, resident_pages{};
    file >> virtual_pages >> resident_pages;
    return resident_pages * sysconf(_SC_PAGESIZE);
}

uint64_t Writes()
{
    std::ifstream file{"/proc/self/io"};
    std::string key;
    uint64_t value;
    while (file >> key >> value) if (key == "write_bytes:") return value;
    return 0;
}

double Cpu()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
        usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

void Sample(const char* phase, CCoinsViewCache& coins, Clock::time_point start, double cpu, uint64_t io)
{
    const auto usage{GetYespowerVerificationCacheUsage()};
    const auto allocator{mallinfo2()};
    std::cout << "{\"phase\":\"" << phase << "\",\"rss\":" << Rss()
              << ",\"proof\":" << usage.proof_storage_bytes
              << ",\"allocator_used\":" << allocator.uordblks
              << ",\"allocator_free\":" << allocator.fordblks
              << ",\"allocator_mmap\":" << allocator.hblkhd
              << ",\"tiers\":" << usage.tiers << ",\"budget\":" << usage.budget_bytes
              << ",\"coins\":" << coins.DynamicMemoryUsage() << ",\"entries\":" << coins.GetCacheSize()
              << ",\"seconds\":" << std::chrono::duration<double>(Clock::now() - start).count()
              << ",\"cpu_seconds\":" << Cpu() - cpu << ",\"write_bytes\":" << Writes() - io << "}" << std::endl;
}

int main(int argc, char** argv)
{
    if (argc != 5 && argc != 6) return 2;
    // Refuse existing paths so this fixture cannot wipe a node or earlier dataset.
    if (fs::exists(fs::path{argv[2]})) {
        std::cerr << "Supply a new private database path\n";
        return 2;
    }
    const std::string mode{argv[1]};
    const uint64_t proofs{std::stoull(argv[3])}, count{std::stoull(argv[4])};
    if ((mode != "none" && mode != "proof" && mode != "coins" && mode != "both") ||
        count == 0 || proofs > UINT32_MAX || (argc == 6 && std::string{argv[5]} != "dirty")) return 2;

    RandomInit();
    InitYespowerVerificationCache(size_t{2048} << 20, true);
    CCoinsViewDB db{DBParams{.path = argv[2], .cache_bytes = 8 << 20}, CoinsViewOptions{}};
    CCoinsViewCache coins{&db};
    coins.SetBestBlock(Key(999));
    auto start{Clock::now()};
    auto cpu{Cpu()};
    auto io{Writes()};

    // Synthetic keys bypass admission only in this allocation-only fixture.
    CBlockHeader header;
    header.nBits = 0x1e0ffff0;
    header.nTime = 1231006505;
    for (uint64_t i{0}; i < proofs; ++i) {
        header.nNonce = i;
        VerificationCache().Insert(VerificationCache().Entry(header));
    }
    for (uint64_t i{0}; i < count; ++i) {
        coins.AddCoin(COutPoint{Txid::FromUint256(Key(i + 1)), 0},
                      Coin{CTxOut{1, CScript{} << OP_TRUE}, 1, false}, false);
    }
    if (argc == 5) {
        coins.Sync();
        std::shared_future<void> settled;
        { LOCK(cs_main); settled = db.CompactFull(); }
        settled.wait();
    }
    // A small dirty tail exercises writes in the otherwise settled comparison.
    for (uint64_t i{count}; i < count + 128; ++i) {
        coins.AddCoin(COutPoint{Txid::FromUint256(Key(i + 1)), 0},
                      Coin{CTxOut{1, CScript{} << OP_TRUE}, 1, false}, false);
    }
    Sample("filled", coins, start, cpu, io);
    start = Clock::now(); cpu = Cpu(); io = Writes();
    if (mode == "proof" || mode == "both") ShrinkYespowerVerificationCache();
    if (mode == "coins" || mode == "both") coins.Flush();
    Sample("cleaned", coins, start, cpu, io);
    start = Clock::now(); cpu = Cpu(); io = Writes();
    malloc_trim(0);
    Sample("trimmed", coins, start, cpu, io);
    start = Clock::now(); cpu = Cpu(); io = Writes();
    uint64_t found{0};
    for (uint64_t i{0}; i < 100000; ++i) {
        found += coins.HaveCoin(COutPoint{Txid::FromUint256(Key((i * 7919) % count + 1)), 0});
    }
    Sample("read_100k", coins, start, cpu, io);
    if (found != 100000) return 3;
    coins.Flush();
    if (!db.HaveCoin(COutPoint{Txid::FromUint256(Key(count + 128)), 0}) || db.GetBestBlock() != Key(999)) return 4;
    return 0;
}
// EOF
