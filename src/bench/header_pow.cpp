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
#include <ctime>
#include <cstdint>
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
static std::atomic<uint64_t> pow_calls{0}, pow_wall_ns{0}, pow_cpu_ns{0};
static bool pow_timing{true};
#if defined(CLOCK_THREAD_CPUTIME_ID)
static std::atomic<bool> pow_cpu_clock_available{true};
#endif

// Unlike elapsed times summed across concurrent calls, this clock measures CPU
// actually consumed by each calling thread. Unsupported platforms say unavailable.
static uint64_t ThreadCPUTimeNs()
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        pow_cpu_clock_available = false;
        return 0;
    }
    return uint64_t(value.tv_sec) * 1'000'000'000 + uint64_t(value.tv_nsec);
#else
    return 0;
#endif
}

static void PrintPoWMeasurements()
{
    std::cout << pow_calls.load() << ',';
    if (!pow_timing) {
        std::cout << "unavailable,unavailable";
        return;
    }
    std::cout << double(pow_wall_ns.load()) / 1e9 << ',';
#if defined(CLOCK_THREAD_CPUTIME_ID)
    if (pow_cpu_clock_available) std::cout << double(pow_cpu_ns.load()) / 1e9;
    else std::cout << "unavailable";
#else
    std::cout << "unavailable";
#endif
}
extern "C" int __real_yespower(yespower_local_t*, const uint8_t*, size_t, const yespower_params_t*, yespower_binary_t*);
extern "C" int __wrap_yespower(yespower_local_t* local, const uint8_t* input, size_t size,
                              const yespower_params_t* params, yespower_binary_t* output)
{
    ++pow_calls;
    if (!pow_timing) return __real_yespower(local, input, size, params, output);
    const auto cpu_start{ThreadCPUTimeNs()};
    const auto start{std::chrono::steady_clock::now()};
    const auto result{__real_yespower(local, input, size, params, output)};
    const auto end{std::chrono::steady_clock::now()};
    const auto cpu_end{ThreadCPUTimeNs()};
    pow_wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    if (cpu_end >= cpu_start) pow_cpu_ns += cpu_end - cpu_start;
    return result;
}
#endif

int main(int argc, char** argv)
{
    try {
        if (argc < 4) throw std::runtime_error("Usage: bench_header_pow RAW_HEADERS COUNT WORKERS (1-8) [CACHE_MIB (1-2048)] [--block-stage] [--grow-cache] [--presync] [--delayed-reuse] [--no-pow-timing] [--stage-timing]");
        bool block_stage{false}, grow_cache{false}, presync{false}, delayed_reuse{false}, stage_timing{false};
        const bool has_cache_arg{argc >= 5 && !std::string_view{argv[4]}.starts_with("--")};
        for (int i = has_cache_arg ? 5 : 4; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--block-stage") block_stage = true;
            else if (option == "--grow-cache") grow_cache = true;
            else if (option == "--presync") presync = true;
            else if (option == "--delayed-reuse") delayed_reuse = true;
            else if (option == "--stage-timing") stage_timing = true;
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            else if (option == "--no-pow-timing") pow_timing = false;
#endif
            else throw std::runtime_error("Unknown mode");
        }
        if (presync && block_stage) throw std::runtime_error("Choose presync or block-stage mode");
        if (delayed_reuse && (presync || block_stage)) throw std::runtime_error("Delayed reuse is a separate proof-cache experiment");
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
        // Identify the fixture independently of PoW call counts. The --presync
        // processed-header column deliberately includes the eight peer copies.
        std::vector<uint256> fixture_ids;
        fixture_ids.reserve(count);
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
            fixture_ids.push_back(previous_hash);
        }
        std::sort(fixture_ids.begin(), fixture_ids.end());
        if (std::adjacent_find(fixture_ids.begin(), fixture_ids.end()) != fixture_ids.end()) {
            throw std::runtime_error("Duplicate header identifier in fixture");
        }
        const size_t unique_fixture_headers{fixture_ids.size()};
        std::vector<uint256>().swap(fixture_ids);
        HeaderPoWVerifier verifier{int(workers)};
        std::cout << "workers,pass,headers,seconds,headers_per_second,yespower_calls,yespower_thread_wall_seconds,yespower_thread_cpu_seconds,unique_fixture_headers";
        if (stage_timing) std::cout << ",verifier_seconds,context_seconds,other_seconds";
        std::cout << std::endl;
        for (const auto* pass : {"cold", "repeat"}) {
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            pow_calls = 0;
            pow_wall_ns = 0;
            pow_cpu_ns = 0;
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
            std::chrono::steady_clock::duration verifier_time{}, context_time{};
            const auto check = [&](std::span<const CBlockHeader> batch) {
                if (!stage_timing) return verifier.Check(batch, params);
                const auto begin{std::chrono::steady_clock::now()};
                const bool valid{verifier.Check(batch, params)};
                verifier_time += std::chrono::steady_clock::now() - begin;
                return valid;
            };
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
                        if (!check(batch)) throw std::runtime_error("Invalid peer proof");
                        // Synthetic chunks deliberately vary below the P2P limit.
                        // Keep requesting data instead of interpreting a short chunk
                        // as the remote tip; this is not a wire-message simulator.
                        const auto context_begin{stage_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
                        const auto result{states[peer]->ProcessNextHeaders(batch, true)};
                        if (stage_timing) context_time += std::chrono::steady_clock::now() - context_begin;
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
                // A repeated identical message can restore individual evidence
                // from the batch cache. Change repeat boundaries only in this
                // opt-in experiment to expose delayed individual-proof misses.
                // This is a controlled reuse pattern, not a measured peer trace.
                const size_t chunk{delayed_reuse && std::string_view{pass} == "repeat" ? 1999U : 2000U};
                for (size_t pos{0}; pos < headers.size(); pos += chunk) {
                    if (!check(std::span{headers}.subspan(pos, std::min(chunk, headers.size() - pos)))) {
                        throw std::runtime_error("Invalid proof of work");
                    }
                }
            }
            const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
            const size_t processed{headers.size() * (presync ? peers : 1)};
            std::cout << workers << ',' << (presync ? "presync-" : "") << pass << ',' << processed << ',' << seconds << ','
                      << processed / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
            PrintPoWMeasurements();
#else
            std::cout << "unavailable,unavailable,unavailable";
#endif
            std::cout << ',' << unique_fixture_headers;
            if (stage_timing) {
                // These caller-thread intervals never overlap. Summed PoW
                // thread timings above are a separate diagnostic, not addends.
                const double verifier_seconds{std::chrono::duration<double>(verifier_time).count()};
                const double context_seconds{std::chrono::duration<double>(context_time).count()};
                std::cout << ',' << verifier_seconds << ',' << context_seconds << ','
                          << seconds - verifier_seconds - context_seconds;
            }
            std::cout << std::endl;
        }
        if (block_stage) {
            // Isolate the serial proof check used by block validation. Reset is
            // a controlled all-miss case, not an assertion about live retention.
            std::cout << "block_proof_pass,headers,seconds,headers_per_second,yespower_calls,yespower_thread_wall_seconds,yespower_thread_cpu_seconds,unique_fixture_headers\n";
            for (const bool reset : {false, true}) {
                if (reset) InitYespowerVerificationCache(cache_bytes, grow_cache);
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                pow_calls = 0;
                pow_wall_ns = 0;
                pow_cpu_ns = 0;
#endif
                const auto start{std::chrono::steady_clock::now()};
                for (const auto& header : headers) {
                    if (!CheckBlockProofOfWork(header, params)) throw std::runtime_error("Invalid block proof");
                }
                const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
                std::cout << (reset ? "cold" : "warm") << ',' << headers.size() << ',' << seconds << ',' << headers.size() / seconds << ',';
#ifdef ENABLE_YESPOWER_BENCH_WRAP
                PrintPoWMeasurements();
#else
                std::cout << "unavailable,unavailable,unavailable";
#endif
                std::cout << ',' << unique_fixture_headers << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
