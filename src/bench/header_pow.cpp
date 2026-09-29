// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// One cold and one repeated pass per process. Unlike a repeated microbenchmark,
// this does not accidentally report a warm verification cache as new PoW work.
#include <chain.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <streams.h>
#include <util/translation.h>
#include <crypto/yespower-1.0.1/yespower.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

const std::function<std::string(const char*)> G_TRANSLATION_FUN{nullptr};

#ifdef ENABLE_YESPOWER_BENCH_WRAP
static std::atomic<uint64_t> pow_calls{0};
extern "C" int __real_yespower(yespower_local_t*, const uint8_t*, size_t, const yespower_params_t*, yespower_binary_t*);
extern "C" int __wrap_yespower(yespower_local_t* local, const uint8_t* input, size_t size,
                              const yespower_params_t* params, yespower_binary_t* output)
{
    ++pow_calls;
    return __real_yespower(local, input, size, params, output);
}
#endif

int main(int argc, char** argv)
{
    try {
        if (argc < 4 || argc > 6) throw std::runtime_error("Usage: bench_header_pow RAW_HEADERS COUNT WORKERS (1-8) [CACHE_MIB (1-2048)] [--block-stage]");
        const bool block_stage{argc == 6};
        if (block_stage && std::string_view{argv[5]} != "--block-stage") throw std::runtime_error("Unknown mode");
        auto number = [](std::string_view arg, unsigned maximum) {
            unsigned value{0};
            auto [end, error]{std::from_chars(arg.data(), arg.data() + arg.size(), value)};
            if (error != std::errc{} || end != arg.data() + arg.size() || !value || value > maximum) {
                throw std::runtime_error("Invalid count/worker limit");
            }
            return value;
        };
        const auto count{number(argv[2], 1'000'000)};
        const auto workers{number(argv[3], MAX_HEADER_POW_WORKERS)};
        const size_t cache_bytes{argc >= 5 ? size_t(number(argv[4], MAX_YESPOWER_CACHE_BYTES >> 20)) << 20 : DEFAULT_YESPOWER_CACHE_BYTES};
        InitYespowerVerificationCache(cache_bytes);
        const auto chain{CChainParams::Main()};
        const auto& params{chain->GetConsensus()};
        std::ifstream input{argv[1], std::ios::binary};
        std::vector<CBlockHeader> headers(count);
        std::deque<CBlockIndex> history;
        history.emplace_back(chain->GenesisBlock());
        auto previous_hash{chain->GenesisBlock().GetHash()};
        for (auto& header : headers) {
            std::array<char, 80> bytes;
            if (!input.read(bytes.data(), bytes.size())) throw std::runtime_error("Truncated header input");
            DataStream stream{std::as_bytes(std::span{bytes})};
            stream >> header;
            if (header.hashPrevBlock != previous_hash ||
                header.nBits != GetNextWorkRequired(&history.back(), &header, params) ||
                header.GetBlockTime() <= history.back().GetMedianTimePast()) {
                throw std::runtime_error("Header continuity/difficulty/time mismatch");
            }
            auto* previous{&history.back()};
            history.emplace_back(header);
            history.back().pprev = previous;
            history.back().nHeight = previous->nHeight + 1;
            previous_hash = header.GetHash();
        }
        HeaderPoWVerifier verifier{int(workers)};
        std::cout << "workers,pass,headers,seconds,headers_per_second,yespower_calls\n";
        for (const auto* pass : {"cold", "repeat"}) {
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            pow_calls = 0;
#endif
            const auto start{std::chrono::steady_clock::now()};
            for (size_t pos{0}; pos < headers.size(); pos += 2000) {
                if (!verifier.Check(std::span{headers}.subspan(pos, std::min(size_t{2000}, headers.size() - pos)), params)) {
                    throw std::runtime_error("Invalid proof of work");
                }
            }
            const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
            std::cout << workers << ',' << pass << ',' << headers.size() << ',' << seconds << ','
                      << headers.size() / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            std::cout << pow_calls.load();
#else
            std::cout << "unavailable";
#endif
            std::cout << std::endl;
        }
        if (block_stage) {
            // Isolate the serial proof check used by block validation. Reset is
            // a controlled all-miss case, not an assertion about live retention.
            std::cout << "block_proof_pass,headers,seconds,headers_per_second,yespower_calls\n";
            for (const bool reset : {false, true}) {
                if (reset) InitYespowerVerificationCache(cache_bytes);
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                pow_calls = 0;
#endif
                const auto start{std::chrono::steady_clock::now()};
                for (const auto& header : headers) {
                    if (!CheckBlockProofOfWork(header, params)) throw std::runtime_error("Invalid block proof");
                }
                const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
                std::cout << (reset ? "cold" : "warm") << ',' << headers.size() << ',' << seconds << ',' << headers.size() / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                std::cout << pow_calls.load();
#else
                std::cout << "unavailable";
#endif
                std::cout << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
