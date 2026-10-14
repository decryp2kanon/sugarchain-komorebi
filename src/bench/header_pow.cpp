// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// One cold and one repeated pass per process. Unlike a repeated microbenchmark,
// this does not accidentally report a warm verification cache as new PoW work.
#include <chain.h>
#include <headerssync.h>
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
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

const std::function<std::string(const char*)> G_TRANSLATION_FUN{nullptr};

#ifdef ENABLE_YESPOWER_BENCH_WRAP
static std::atomic<uint64_t> pow_calls{0}, pow_wall_ns{0};
extern "C" int __real_yespower(yespower_local_t*, const uint8_t*, size_t, const yespower_params_t*, yespower_binary_t*);
extern "C" int __wrap_yespower(yespower_local_t* local, const uint8_t* input, size_t size,
                              const yespower_params_t* params, yespower_binary_t* output)
{
    ++pow_calls;
    const auto start{std::chrono::steady_clock::now()};
    const auto result{__real_yespower(local, input, size, params, output)};
    pow_wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return result;
}
#endif

int main(int argc, char** argv)
{
    try {
        if (argc < 4) throw std::runtime_error("Usage: bench_header_pow RAW_HEADERS COUNT WORKERS (1-8) [CACHE_MIB (1-2048)] [--block-stage] [--grow-cache] [--presync]");
        bool block_stage{false}, grow_cache{false}, presync{false};
        const bool has_cache_arg{argc >= 5 && !std::string_view{argv[4]}.starts_with("--")};
        for (int i = has_cache_arg ? 5 : 4; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--block-stage") block_stage = true;
            else if (option == "--grow-cache") grow_cache = true;
            else if (option == "--presync") presync = true;
            else throw std::runtime_error("Unknown mode");
        }
        if (presync && block_stage) throw std::runtime_error("Choose presync or block-stage mode");
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
        const size_t cache_bytes{has_cache_arg ? size_t(number(argv[4], MAX_YESPOWER_CACHE_BYTES >> 20)) << 20 : DEFAULT_YESPOWER_CACHE_BYTES};
        InitYespowerVerificationCache(cache_bytes, grow_cache);
        const auto chain{CChainParams::Main()};
        const auto& params{chain->GetConsensus()};
        std::ifstream input{argv[1], std::ios::binary};
        std::vector<CBlockHeader> headers(count);
        std::deque<CBlockIndex> history;
        history.emplace_back(chain->GenesisBlock());
        const auto genesis_hash{chain->GenesisBlock().GetHash()};
        history.front().phashBlock = &genesis_hash;
        history.front().nChainWork = GetBlockProof(history.front());
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
            history.back().nChainWork = previous->nChainWork + GetBlockProof(header);
            previous_hash = header.GetHash();
        }
        HeaderPoWVerifier verifier{int(workers)};
        std::cout << "workers,pass,headers,seconds,headers_per_second,yespower_calls,yespower_thread_wall_seconds\n";
        for (const auto* pass : {"cold", "repeat"}) {
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            pow_calls = 0;
            pow_wall_ns = 0;
#endif
            // Independent peer states, bounded/misaligned messages and shared proof
            // evidence. Includes genuine PoW and contextual checks, but not socket
            // delivery, net_processing's index lookup, or the REDOWNLOAD phase.
            constexpr size_t peers{8};
            std::vector<std::unique_ptr<HeadersSyncState>> states;
            std::array<size_t, peers> positions{};
            if (presync) {
                for (size_t peer = 0; peer < peers; ++peer) {
                    states.push_back(std::make_unique<HeadersSyncState>(peer, params,
                        chain->HeadersSync(), history.front(), history.back().nChainWork));
                }
            }
            const auto start{std::chrono::steady_clock::now()};
            if (presync) {
                bool pending{true};
                while (pending) {
                    pending = false;
                    for (size_t peer = 0; peer < peers; ++peer) {
                        auto& pos = positions[peer];
                        if (pos == headers.size()) continue;
                        pending = true;
                        const size_t count{std::min(size_t{1000} + peer * 125, headers.size() - pos)};
                        const auto batch{std::span{headers}.subspan(pos, count)};
                        if (!verifier.Check(batch, params)) throw std::runtime_error("Invalid peer proof");
                        // Synthetic chunks deliberately vary below the P2P limit.
                        // Keep requesting data instead of interpreting a short chunk
                        // as the remote tip; this is not a wire-message simulator.
                        const auto result{states[peer]->ProcessNextHeaders(batch, true)};
                        pos += count;
                        if (!result.success || !result.pow_validated_headers.empty() ||
                            states[peer]->GetPresyncWork() != history[pos].nChainWork) {
                            throw std::runtime_error("Presync state/work mismatch");
                        }
                    }
                }
                for (const auto& state : states) {
                    if (state->GetState() != HeadersSyncState::State::REDOWNLOAD) {
                        throw std::runtime_error("Missing presync transition");
                    }
                }
            } else {
                for (size_t pos{0}; pos < headers.size(); pos += 2000) {
                    if (!verifier.Check(std::span{headers}.subspan(pos, std::min(size_t{2000}, headers.size() - pos)), params)) {
                        throw std::runtime_error("Invalid proof of work");
                    }
                }
            }
            const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
            const size_t processed{headers.size() * (presync ? peers : 1)};
            std::cout << workers << ',' << (presync ? "presync-" : "") << pass << ',' << processed << ',' << seconds << ','
                      << processed / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            std::cout << pow_calls.load() << ',' << double(pow_wall_ns.load()) / 1e9;
#else
            std::cout << "unavailable,unavailable";
#endif
            std::cout << std::endl;
        }
        if (block_stage) {
            // Isolate the serial proof check used by block validation. Reset is
            // a controlled all-miss case, not an assertion about live retention.
            std::cout << "block_proof_pass,headers,seconds,headers_per_second,yespower_calls,yespower_thread_wall_seconds\n";
            for (const bool reset : {false, true}) {
                if (reset) InitYespowerVerificationCache(cache_bytes, grow_cache);
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                pow_calls = 0;
                pow_wall_ns = 0;
#endif
                const auto start{std::chrono::steady_clock::now()};
                for (const auto& header : headers) {
                    if (!CheckBlockProofOfWork(header, params)) throw std::runtime_error("Invalid block proof");
                }
                const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
                std::cout << (reset ? "cold" : "warm") << ',' << headers.size() << ',' << seconds << ',' << headers.size() / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                std::cout << pow_calls.load() << ',' << double(pow_wall_ns.load()) / 1e9;
#else
                std::cout << "unavailable,unavailable";
#endif
                std::cout << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
