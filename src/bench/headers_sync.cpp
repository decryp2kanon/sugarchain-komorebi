// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Isolated PRESYNC/REDOWNLOAD cost, after genuine PoW checks outside the timer.
// The threshold is the fixture's chainwork, not a production trust-anchor change.
#include <headerssync.h>
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
        if (argc != 4) throw std::runtime_error("Usage: bench_headers_sync RAW_HEADERS COUNT ROUNDS");
        auto number = [](std::string_view arg, unsigned maximum) {
            unsigned value{0};
            auto [end, error]{std::from_chars(arg.data(), arg.data() + arg.size(), value)};
            if (error != std::errc{} || end != arg.data() + arg.size() || !value || value > maximum) {
                throw std::runtime_error("Invalid count/rounds");
            }
            return value;
        };
        const auto count{number(argv[2], 1'000'000)};
        const auto rounds{number(argv[3], 1000)};
        const auto chain{CChainParams::Main()};
        const auto& params{chain->GetConsensus()};
        std::ifstream input{argv[1], std::ios::binary};
        std::vector<CBlockHeader> headers(count);
        std::deque<uint256> hashes;
        std::deque<CBlockIndex> history;
        auto append = [&](const CBlockHeader& header) {
            auto* previous{history.empty() ? nullptr : &history.back()};
            hashes.push_back(header.GetHash());
            history.emplace_back(header);
            auto& index{history.back()};
            index.phashBlock = &hashes.back();
            index.pprev = previous;
            index.nHeight = history.size() - 1;
            index.nChainWork = (previous ? previous->nChainWork : arith_uint256{0}) + GetBlockProof(header);
        };
        append(chain->GenesisBlock());
        for (auto& header : headers) {
            std::array<char, 80> bytes;
            if (!input.read(bytes.data(), bytes.size())) throw std::runtime_error("Truncated input");
            DataStream stream{std::as_bytes(std::span{bytes})};
            stream >> header;
            if (header.hashPrevBlock != hashes.back() ||
                header.nBits != GetNextWorkRequired(&history.back(), &header, params) ||
                header.GetBlockTime() <= history.back().GetMedianTimePast()) {
                throw std::runtime_error("Header context mismatch");
            }
            append(header);
        }
        HeaderPoWVerifier verifier{8};
        if (!verifier.Check(headers, params)) throw std::runtime_error("Invalid PoW");
        const auto start{std::chrono::steady_clock::now()};
        for (unsigned round{0}; round < rounds; ++round) {
            HeadersSyncState sync{0, params, chain->HeadersSync(), history.front(), history.back().nChainWork};
            size_t returned{0};
            for (int pass{0}; pass < 2; ++pass) {
                for (size_t pos{0}; pos < headers.size(); pos += 2000) {
                    const auto result{sync.ProcessNextHeaders(std::span{headers}.subspan(pos, std::min(size_t{2000}, headers.size() - pos)), true)};
                    if (!result.success) throw std::runtime_error("Header sync failed");
                    for (const auto& header : result.pow_validated_headers) {
                        if (pass == 0 || returned >= headers.size() || header.GetHash() != hashes[++returned]) {
                            throw std::runtime_error("Returned headers differ");
                        }
                    }
                }
                if (sync.GetState() != (pass == 0 ? HeadersSyncState::State::REDOWNLOAD : HeadersSyncState::State::FINAL)) {
                    throw std::runtime_error("Unexpected sync state");
                }
            }
            if (returned != headers.size()) throw std::runtime_error("Missing returned headers");
        }
        const double seconds{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
        std::cout << "component,headers,rounds,seconds,headers_per_second\n"
                  << "presync_redownload," << count << ',' << rounds << ',' << seconds << ','
                  << uint64_t(count) * rounds * 2 / seconds << std::endl;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
