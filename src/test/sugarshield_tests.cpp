// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/params.h>
#include <headerssync.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/data/sugarchain_headers.raw.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
using State = HeadersSyncState::State;

// Stable addresses for both the full index chain and its block hashes. No
// production private members or test-only production hooks are needed.
struct HeaderChain {
    std::deque<uint256> hashes;
    std::deque<CBlockIndex> index;
    std::vector<CBlockHeader> headers; // Includes genesis at position zero.

    explicit HeaderChain(const CBlockHeader& genesis) { Append(genesis); }
    HeaderChain(const HeaderChain&) = delete;
    HeaderChain& operator=(const HeaderChain&) = delete;

    void Append(const CBlockHeader& header)
    {
        CBlockIndex* previous{index.empty() ? nullptr : &index.back()};
        hashes.push_back(header.GetHash());
        index.emplace_back(header);
        auto& entry = index.back();
        entry.pprev = previous;
        entry.nHeight = index.size() - 1;
        entry.phashBlock = &hashes.back();
        entry.nChainWork = (previous ? previous->nChainWork : arith_uint256{0}) + GetBlockProof(header);
        headers.push_back(header);
    }
};

const std::vector<CBlockHeader>& MainnetHeaders()
{
    static const auto headers = [] {
        BOOST_REQUIRE_EQUAL(test::data::sugarchain_headers.size(), 6000U * 80);
        DataStream stream{test::data::sugarchain_headers};
        std::vector<CBlockHeader> result(6000);
        for (auto& header : result) stream >> header;
        BOOST_REQUIRE(stream.empty());
        return result;
    }();
    return headers;
}

void MineHeader(CBlockHeader& header, const Consensus::Params& params)
{
    // Synthetic fixtures use SHA256d with the real SugarShield parameters and
    // powLimit. The real mainnet fixture separately exercises YespowerSugar.
    BOOST_REQUIRE(!params.fYespowerSugar);
    for (uint32_t attempts{0}; attempts < 1'000'000; ++attempts, ++header.nNonce) {
        if (CheckBlockProofOfWork(header, params)) return;
    }
    BOOST_FAIL("Failed to mine deterministic synthetic header");
}

void ExtendChain(HeaderChain& chain, size_t last_height, const Consensus::Params& params, uint256 merkle = uint256::ZERO)
{
    while (chain.headers.size() <= last_height) {
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = chain.hashes.back();
        header.hashMerkleRoot = merkle;
        // Alternate 200-block runs at four/six seconds. Difficulty must change,
        // so a stale/reset/truncated context cannot hide behind a constant target.
        header.nTime = chain.headers.back().nTime + (chain.headers.size() / 200 % 2 ? 6 : 4);
        header.nBits = GetNextWorkRequired(&chain.index.back(), &header, params);
        MineHeader(header, params);
        chain.Append(header);
    }
}

// Run the actual two-pass state machine, checking every returned header as well
// as work/height/locator state. A one-header message is marked full deliberately,
// just as in upstream headers_sync_chainwork_tests, to exercise message boundaries.
void CheckRoundTrip(const HeaderChain& chain, size_t start, const Consensus::Params& params,
                    const HeadersSyncParams& sync_params, size_t batch_size)
{
    HeadersSyncState sync{0, params, sync_params, chain.index.at(start), chain.index.back().nChainWork};
    for (size_t pos = start + 1; pos < chain.headers.size();) {
        const size_t count{std::min(batch_size, chain.headers.size() - pos)};
        const auto result{sync.ProcessNextHeaders(std::span{chain.headers}.subspan(pos, count), true)};
        BOOST_REQUIRE(result.success);
        BOOST_CHECK(result.request_more);
        BOOST_CHECK(result.pow_validated_headers.empty());
        pos += count;
        BOOST_CHECK_EQUAL(sync.GetPresyncHeight(), pos - 1);
        BOOST_CHECK(sync.GetPresyncWork() == chain.index[pos - 1].nChainWork);
        BOOST_REQUIRE(sync.GetState() == (pos == chain.headers.size() ? State::REDOWNLOAD : State::PRESYNC));
    }
    BOOST_REQUIRE(sync.GetState() == State::REDOWNLOAD);
    BOOST_CHECK(sync.NextHeadersRequestLocator().vHave.front() == chain.hashes[start]);

    size_t returned{start + 1};
    for (size_t pos = start + 1; pos < chain.headers.size();) {
        const size_t count{std::min(batch_size, chain.headers.size() - pos)};
        const auto result{sync.ProcessNextHeaders(std::span{chain.headers}.subspan(pos, count), true)};
        BOOST_REQUIRE(result.success);
        for (const auto& header : result.pow_validated_headers) {
            BOOST_REQUIRE_LT(returned, chain.headers.size());
            BOOST_CHECK(header.GetHash() == chain.hashes[returned++]);
        }
        pos += count;
        const bool finished{pos == chain.headers.size()};
        BOOST_CHECK_EQUAL(result.request_more, !finished);
        BOOST_REQUIRE(sync.GetState() == (finished ? State::FINAL : State::REDOWNLOAD));
    }
    BOOST_CHECK_EQUAL(returned, chain.headers.size());
}

struct SugarShieldSetup : BasicTestingSetup {
    const std::unique_ptr<const CChainParams> main{CChainParams::Main()};
    const Consensus::Params& params{main->GetConsensus()};

    SugarShieldSetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-debug=net", "-loglevel=debug"}}}
    {
        // Deterministic commitment salts and a fixed time well after the fixture.
        SeedRandomForTest(SeedRand::ZEROS);
        SetMockTime(1'800'000'000);
    }

    Consensus::Params SyntheticParams() const
    {
        auto result{params};
        result.fYespowerSugar = false;
        return result;
    }

    CBlockHeader SyntheticGenesis() const
    {
        CBlockHeader header{main->GenesisBlock()};
        MineHeader(header, SyntheticParams());
        return header;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(sugarshield_tests, SugarShieldSetup)

BOOST_AUTO_TEST_CASE(calculation_vectors)
{
    BOOST_CHECK_EQUAL(params.nPowAveragingWindow, 510);
    BOOST_CHECK_EQUAL(params.nPowTargetSpacing, 5);
    BOOST_CHECK_EQUAL(params.nPowMaxAdjustUp, 16);
    BOOST_CHECK_EQUAL(params.nPowMaxAdjustDown, 32);
    BOOST_CHECK_EQUAL(CBlockIndex::nMedianTimeSpan, 11);
    BOOST_CHECK_EQUAL(params.AveragingWindowTimespan(), 2550);
    BOOST_CHECK_EQUAL(params.MinActualTimespan(), 2142);
    BOOST_CHECK_EQUAL(params.MaxActualTimespan(), 3366);

    // Fixed answers, not a second invocation of the production formula as an
    // oracle. See data/sugarchain_headers.md for integer arithmetic and provenance.
    struct Vector { size_t count; uint32_t bits; uint32_t spacing; uint32_t expected; };
    const Vector vectors[]{
        {1,   0x1e123456,   5, 0x1f3fffff},
        {509, 0x1e123456,   5, 0x1f3fffff},
        {510, 0x1e123456,   5, 0x1f3fffff},
        {511, 0x1e123456,   5, 0x1e12295e}, // Older MTP contains only genesis.
        {520, 0x1e123456,   5, 0x1e123282}, // Older MTP has 10 entries (upper median).
        {521, 0x1e123456,   5, 0x1e123455},
        {522, 0x1e123456,   5, 0x1e123455},
        {700, 0x1e123456,   1, 0x1e0f4aae}, // Minimum timespan clamp.
        {700, 0x1e123456, 100, 0x1e1807a4}, // Maximum timespan clamp.
        {700, 0x1f3fffff, 100, 0x1f3fffff}, // powLimit clamp.
        {700, 0x03012345,   5, 0x030120de}, // Divide-before-multiply rounding.
    };
    for (const auto& v : vectors) {
        BOOST_TEST_CONTEXT("count=" << v.count << " bits=" << v.bits << " spacing=" << v.spacing) {
            CBlockHeader header{main->GenesisBlock()};
            header.nBits = v.bits;
            HeaderChain chain{header};
            for (size_t i = 1; i < v.count; ++i) {
                header.nTime += v.spacing;
                chain.Append(header);
            }
            BOOST_CHECK_EQUAL(GetNextWorkRequired(&chain.index.back(), nullptr, params), v.expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(averaging_membership_and_mtp_clamp_boundaries)
{
    CBlockHeader header{main->GenesisBlock()};
    header.nBits = 0x1e123456;
    HeaderChain chain{header};
    for (size_t i = 1; i < 600; ++i) {
        header.nTime += 5;
        chain.Append(header);
    }
    const auto work = [&] { return GetNextWorkRequired(&chain.index.back(), nullptr, params); };
    BOOST_CHECK_EQUAL(work(), 0x1e123455U);
    // Tip 599 averages [90, 599]; height 89 contributes time, not a target.
    chain.index[89].nBits = 0x1e100000;
    BOOST_CHECK_EQUAL(work(), 0x1e123455U);
    chain.index[90].nBits = 0x1e100000;
    BOOST_CHECK_EQUAL(work(), 0x1e12333aU);
    chain.index[90].nBits = 0x1e123456;
    chain.index[599].nBits = 0x1e100000;
    BOOST_CHECK_EQUAL(work(), 0x1e12333aU);
    chain.index[599].nBits = 0x1e123456;

    // Exercise integer truncation at both clamp boundaries with controlled MTPs.
    // These are calculator inputs, not claims of contextually valid block times.
    const std::pair<uint32_t, uint32_t> vectors[]{
        {917, 0x1e0f4aae}, {918, 0x1e0f4aae}, {919, 0x1e0f4c82},
        {5813, 0x1e1805d0}, {5814, 0x1e1807a4}, {5815, 0x1e1807a4},
    };
    for (const auto& [span, expected] : vectors) {
        for (size_t i = 79; i <= 89; ++i) chain.index[i].nTime = 1'600'000'000;
        for (size_t i = 589; i <= 599; ++i) chain.index[i].nTime = 1'600'000'000 + span;
        BOOST_CHECK_EQUAL(work(), expected);
        // Outliers at the endpoints must not replace the median-of-11 values.
        chain.index[89].nTime -= 10'000;
        chain.index[599].nTime += 10'000;
        BOOST_CHECK_EQUAL(work(), expected);
    }
}

BOOST_AUTO_TEST_CASE(history_521_is_sufficient_and_520_is_not)
{
    CBlockHeader header{main->GenesisBlock()};
    header.nBits = 0x1e123456;
    HeaderChain chain{header};
    size_t shorter_history_mismatches{0};
    for (size_t count = 1; count <= 2500; ++count) {
        if (count > 1) {
            header.nTime += 5;
            chain.Append(header);
        }
        const auto full{GetNextWorkRequired(&chain.index.back(), nullptr, params)};
        for (const size_t retained : {510U, 511U, 520U, 521U}) {
            // Temporarily hide older ancestors, restoring the pointer before any
            // assertion. This models a truncated history without copying the
            // production deque/append/trimming implementation into the test.
            auto& first = chain.index[count > retained ? count - retained : 0];
            auto* saved{std::exchange(first.pprev, nullptr)};
            const auto bounded{GetNextWorkRequired(&chain.index.back(), nullptr, params)};
            first.pprev = saved;
            BOOST_TEST_CONTEXT("count=" << count << " retained=" << retained) {
                if (retained == 521 || count <= retained) BOOST_CHECK_EQUAL(bounded, full);
                if (count == 521 && retained == 520) {
                    BOOST_CHECK_EQUAL(full, 0x1e123455U);
                    BOOST_CHECK_EQUAL(bounded, 0x1e123282U);
                }
                if (count > 520 && retained == 520 && bounded != full) ++shorter_history_mismatches;
            }
        }
    }
    BOOST_CHECK_GT(shorter_history_mismatches, 0U);
}

BOOST_AUTO_TEST_CASE(presync_redownload_sliding_and_small_buffers)
{
    const auto synthetic{SyntheticParams()};
    HeaderChain chain{SyntheticGenesis()};
    ExtendChain(chain, 2600, synthetic);
    BOOST_REQUIRE(std::adjacent_find(chain.headers.begin(), chain.headers.end(),
        [](const auto& a, const auto& b) { return a.nBits != b.nBits; }) != chain.headers.end());
    // Covers history sizes 510/511/520/521, eviction on entry 522, and thousands
    // of subsequent evictions inside the actual production state machine.
    for (const size_t buffer : {0U, 1U, 520U, 521U, 15218U}) {
        BOOST_TEST_CONTEXT("buffer=" << buffer) {
            CheckRoundTrip(chain, 0, synthetic, {1, buffer}, 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(non_genesis_start_and_reset)
{
    const auto synthetic{SyntheticParams()};
    HeaderChain chain{SyntheticGenesis()};
    ExtendChain(chain, 2600, synthetic);
    for (const size_t start : {1U, 509U, 510U, 519U, 520U, 521U, 1400U}) {
        BOOST_TEST_CONTEXT("start=" << start) {
            CheckRoundTrip(chain, start, synthetic, {1, 17}, 17);
        }
    }
}

BOOST_AUTO_TEST_CASE(invalid_difficulty_with_valid_pow_in_both_passes)
{
    const auto synthetic{SyntheticParams()};
    HeaderChain chain{SyntheticGenesis()};
    ExtendChain(chain, 1800, synthetic);
    for (const size_t height : {1U, 510U, 511U, 520U, 521U, 522U, 1400U}) {
        auto bad{chain.headers[height]};
        --bad.nBits;
        MineHeader(bad, synthetic);
        BOOST_REQUIRE(CheckBlockProofOfWork(bad, synthetic));
        BOOST_REQUIRE_NE(bad.nBits, GetNextWorkRequired(&chain.index[height - 1], &bad, synthetic));
        for (const bool redownload : {false, true}) {
            BOOST_TEST_CONTEXT("height=" << height << " redownload=" << redownload) {
                HeadersSyncState sync{0, synthetic, {1, 15218}, chain.index.front(), chain.index.back().nChainWork};
                if (redownload) {
                    BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{chain.headers}.subspan(1), true).success);
                    BOOST_REQUIRE(sync.GetState() == State::REDOWNLOAD);
                }
                if (height > 1) {
                    BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{chain.headers}.subspan(1, height - 1), true).success);
                }
                // This diagnostic is emitted only when the difficulty helper
                // returns false, before any REDOWNLOAD commitment comparison.
                ASSERT_DEBUG_LOG("invalid difficulty transition at height=" + std::to_string(height) +
                                 (redownload ? " (redownload phase)" : " (presync phase)"));
                const auto result{sync.ProcessNextHeaders(std::span{&bad, 1}, true)};
                BOOST_CHECK(!result.success);
                BOOST_CHECK(!result.request_more);
                BOOST_CHECK(result.pow_validated_headers.empty());
                BOOST_CHECK(sync.GetState() == State::FINAL);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(redownload_commitment_rejects_different_valid_chain)
{
    const auto synthetic{SyntheticParams()};
    HeaderChain first{SyntheticGenesis()};
    HeaderChain other{SyntheticGenesis()};
    ExtendChain(first, 1300, synthetic);
    ExtendChain(other, 1300, synthetic, uint256::ONE);
    // Both chains have identical timestamps, correct difficulty and valid PoW.
    // With period one, the alternate chain must match every salted commitment;
    // the fixed RNG seed makes the mismatching sequence reproducible.
    HeadersSyncState sync{0, synthetic, {1, 15218}, first.index.front(), first.index.back().nChainWork};
    BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{first.headers}.subspan(1), true).success);
    BOOST_REQUIRE(sync.GetState() == State::REDOWNLOAD);
    ASSERT_DEBUG_LOG("commitment mismatch");
    // Stay below the work threshold so that the check cannot be skipped by the
    // legitimate final-chainwork fast path.
    const auto result{sync.ProcessNextHeaders(std::span{other.headers}.subspan(1, 128), true)};
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.request_more);
    BOOST_CHECK(result.pow_validated_headers.empty());
    BOOST_CHECK(sync.GetState() == State::FINAL);
}

BOOST_AUTO_TEST_CASE(mainnet_6000_headers_pow_difficulty_and_two_pass_sync)
{
    HeaderChain chain{main->GenesisBlock()};
    BOOST_REQUIRE(params.fYespowerSugar);
    BOOST_REQUIRE(chain.hashes.front() == uint256{"7d5eaec2dbb75f99feadfa524c78b7cabc1d8c8204f79d4f3a83381b811b0adc"});
    BOOST_REQUIRE(CheckBlockProofOfWork(chain.headers.front(), params));
    for (const auto& header : MainnetHeaders()) {
        BOOST_TEST_CONTEXT("mainnet height=" << chain.headers.size()) {
            BOOST_REQUIRE(header.hashPrevBlock == chain.hashes.back());
            // The recorded mainnet nBits is the independent consensus oracle.
            BOOST_REQUIRE_EQUAL(GetNextWorkRequired(&chain.index.back(), &header, params), header.nBits);
            BOOST_REQUIRE(CheckBlockProofOfWork(header, params));
            chain.Append(header);
        }
    }
    BOOST_CHECK(chain.hashes.back() == uint256{"e7a04205f70e5b6e99d83a8f720748fee559a382701b39ff9891e391e6cf81d9"});
    BOOST_CHECK_EQUAL(chain.headers.at(511).nBits, 0x1f3fffffU);
    BOOST_CHECK_EQUAL(chain.headers.at(512).nBits, 0x1f35c28eU);
    CheckRoundTrip(chain, 0, params, main->HeadersSync(), 2000);
    CheckRoundTrip(chain, 4000, params, main->HeadersSync(), 17);
}

BOOST_AUTO_TEST_CASE(zero_window_uses_bitcoin_transition_rules)
{
    auto bitcoin_style{SyntheticParams()};
    bitcoin_style.nPowAveragingWindow = 0;
    bitcoin_style.fPowAllowMinDifficultyBlocks = false;
    bitcoin_style.fPowNoRetargeting = false;
    HeaderChain chain{SyntheticGenesis()};
    ExtendChain(chain, 700, bitcoin_style);
    BOOST_CHECK_EQUAL(chain.headers.back().nBits, chain.headers.front().nBits);
    CheckRoundTrip(chain, 0, bitcoin_style, {1, 7}, 1);
    auto bad{chain.headers[1]};
    --bad.nBits;
    MineHeader(bad, bitcoin_style);
    BOOST_REQUIRE(CheckBlockProofOfWork(bad, bitcoin_style));
    BOOST_REQUIRE(!PermittedDifficultyTransition(bitcoin_style, 1, chain.headers[0].nBits, bad.nBits));
    HeadersSyncState sync{0, bitcoin_style, {1, 7}, chain.index.front(), chain.index.back().nChainWork};
    BOOST_CHECK(!sync.ProcessNextHeaders(std::span{&bad, 1}, true).success);
    BOOST_CHECK(sync.GetState() == State::FINAL);
}

BOOST_AUTO_TEST_SUITE_END()
