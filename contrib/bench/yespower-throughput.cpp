// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Adapted from official Sugarchain PR #225's raw throughput diagnostic.
// Link with BUILD/lib/libbitcoin_crypto.a using -O2 -std=c++20 -pthread -Isrc.
// Run: /tmp/yespower-throughput src/test/data/sugarchain_headers.raw [MAX_WORKERS]
// Uses no node, network or datadir. This is NOT a full IBD measurement.

#include <crypto/yespower-1.0.1/yespower.h>
#include <crypto/yespower-1.0.1/sha256.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("Supply the 6000-header fixture path and optional maximum workers");
        int max_workers{16};
        if (argc == 3) {
            size_t used{0};
            max_workers = std::stoi(argv[2], &used);
            if (used != std::strlen(argv[2]) || max_workers < 1 || max_workers > 16) {
                throw std::runtime_error("Maximum workers must be between 1 and 16");
            }
        }
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read header fixture");
        std::vector<std::array<uint8_t, 80>> headers(6000);
        for (auto& header : headers) {
            if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
                throw std::runtime_error("Incomplete header fixture");
            }
        }
        if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected trailing data");

        static constexpr uint8_t personal[]{
            "Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote"};
        const yespower_params_t params{YESPOWER_1_0, 2048, 32, personal, sizeof(personal) - 1};
        std::vector<yespower_binary_t> reference(headers.size());
        std::cout << "workers,headers,seconds,hashes_per_second,identical_to_serial,results_sha256\n";
        for (unsigned workers : {1, 2, 4, 8, 16}) {
            if (workers > static_cast<unsigned>(max_workers)) break;
            std::vector<yespower_binary_t> results(headers.size());
            std::atomic<size_t> next{0};
            std::atomic<bool> failed{false};
            const auto begin{std::chrono::steady_clock::now()};
            {
                std::vector<std::jthread> threads;
                for (unsigned t{0}; t < workers; ++t) {
                    threads.emplace_back([&] {
                        // Explicit local storage mirrors upstream's diagnostic;
                        // each worker owns its scratch memory until it exits.
                        yespower_local_t local;
                        if (yespower_init_local(&local)) {
                            failed = true;
                            return;
                        }
                        for (;;) {
                            const size_t i{next.fetch_add(1, std::memory_order_relaxed)};
                            if (i >= headers.size() || failed.load()) break;
                            if (yespower(&local, headers[i].data(), headers[i].size(), &params, &results[i])) {
                                failed = true;
                                break;
                            }
                        }
                        if (yespower_free_local(&local)) failed = true;
                    });
                }
            } // Join before reading results or destroying header/scratch owners.
            const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()};
            if (failed) throw std::runtime_error("Yespower failed");
            if (workers == 1) reference = results;
            for (size_t i{0}; i < headers.size(); ++i) {
                if (std::memcmp(reference[i].uc, results[i].uc, sizeof(results[i].uc))) {
                    throw std::runtime_error("Parallel result differs at header " + std::to_string(i + 1));
                }
            }
            SHA256_CTX context;
            SHA256_Init(&context);
            for (const auto& result : results) SHA256_Update(&context, result.uc, sizeof(result.uc));
            std::array<uint8_t, 32> digest;
            SHA256_Final(digest.data(), &context);
            std::cout << workers << ',' << headers.size() << ',' << seconds << ','
                      << headers.size() / seconds << ",true,";
            for (uint8_t byte : digest) std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
            std::cout << std::dec << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
