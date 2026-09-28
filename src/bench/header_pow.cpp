// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// One cold and one repeated pass per process. Unlike a repeated microbenchmark,
// this does not accidentally report a warm verification cache as new PoW work.
#include <chain.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <streams.h>
#include <util/translation.h>

#include <algorithm>
#include <array>
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

int main(int argc, char** argv)
{
    try {
        if (argc != 4) throw std::runtime_error("Usage: bench_header_pow RAW_HEADERS COUNT WORKERS (1-8)");
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
        std::cout << "workers,pass,headers,seconds,headers_per_second\n";
        for (const auto* pass : {"cold", "repeat"}) {
            const auto start{std::chrono::steady_clock::now()};
            for (size_t pos{0}; pos < headers.size(); pos += 2000) {
                if (!verifier.Check(std::span{headers}.subspan(pos, std::min(size_t{2000}, headers.size() - pos)), params)) {
                    throw std::runtime_error("Invalid proof of work");
                }
            }
            const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
            std::cout << workers << ',' << pass << ',' << headers.size() << ',' << seconds << ','
                      << headers.size() / seconds << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
