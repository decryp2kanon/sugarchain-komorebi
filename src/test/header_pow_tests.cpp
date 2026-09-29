// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <crypto/yespower-1.0.1/yespower.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/data/sugarchain_headers.raw.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> observing{false};
std::atomic<unsigned> calls{0}, active{0}, peak{0}, fail_call{0};

struct Observation {
    explicit Observation(unsigned fail = 0)
    {
        BOOST_REQUIRE_EQUAL(active.load(), 0U);
        calls = peak = 0;
        fail_call = fail;
        observing = true;
    }
    ~Observation() { observing = false; }
};

struct CacheBudget {
    explicit CacheBudget(size_t bytes) { InitYespowerVerificationCache(bytes); }
    ~CacheBudget() { InitYespowerVerificationCache(DEFAULT_YESPOWER_CACHE_BYTES); }
};

struct HeaderPoWSetup : BasicTestingSetup {
    const std::unique_ptr<const CChainParams> main{CChainParams::Main()};
    Consensus::Params params{main->GetConsensus()};

    HeaderPoWSetup() : BasicTestingSetup{ChainType::REGTEST}
    {
        // Easy, genuine Yespower proofs. These fixtures test PoW scheduling,
        // not a SugarShield chain; the real-chain tests cover contextual rules.
        params.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    }

    std::vector<CBlockHeader> Headers(unsigned tag, unsigned count) const
    {
        std::vector<CBlockHeader> headers(count, main->GenesisBlock());
        for (unsigned i{0}; i < count; ++i) {
            auto& h{headers[i]};
            h.nTime += tag * 1000 + i;
            h.nBits = 0x207fffff;
            h.nNonce = 0;
            // The uncached primitive supplies the oracle without pre-populating
            // the production verification cache tested below.
            while (!CheckProofOfWorkImpl(h.GetPoWHash(), h.nBits, params)) ++h.nNonce;
        }
        return headers;
    }

    void Invalidate(CBlockHeader& header) const
    {
        do { ++header.nNonce; } while (CheckProofOfWorkImpl(header.GetPoWHash(), header.nBits, params));
    }
};
} // namespace

#ifdef ENABLE_YESPOWER_TEST_WRAP
extern "C" int __real_yespower(yespower_local_t*, const uint8_t*, size_t, const yespower_params_t*, yespower_binary_t*);
extern "C" int __wrap_yespower(yespower_local_t* local, const uint8_t* input, size_t size,
                             const yespower_params_t* params, yespower_binary_t* output)
{
    if (!observing.load()) return __real_yespower(local, input, size, params, output);
    const auto ordinal{++calls};
    const auto concurrent{++active};
    auto maximum{peak.load()};
    while (maximum < concurrent && !peak.compare_exchange_weak(maximum, concurrent)) {}
    // Simulate the library's documented local resource failure, never success.
    const int result{ordinal == fail_call.load() ? -1 : __real_yespower(local, input, size, params, output)};
    --active;
    return result;
}
#endif

BOOST_FIXTURE_TEST_SUITE(header_pow_tests, HeaderPoWSetup)

BOOST_FIXTURE_TEST_CASE(indexed_proofs_survive_eviction_but_not_disk_copies, TestingSetup)
{
    CacheBudget budget{DEFAULT_YESPOWER_CACHE_BYTES};
    auto& chainman{*m_node.chainman};
    const auto& params{chainman.GetConsensus()};
    DataStream stream{test::data::sugarchain_headers};
    std::vector<CBlockHeader> headers(6000);
    for (auto& header : headers) stream >> header;
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, params));
    BlockValidationState state;
    // Isolate the post-minimum-work validation layer; P2P/PRESYNC tests cover
    // the caller's chainwork precondition without lowering the mainnet value.
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(headers, true, state, nullptr));
    LOCK(cs_main);
    InitYespowerVerificationCache(64);
    {
        Observation observation;
        for (const auto& header : headers) {
            const auto* index{chainman.m_blockman.LookupBlockIndex(header.GetHash())};
            BOOST_REQUIRE(index);
            CacheVerifiedBlockIndexProof(*index, params);
            BOOST_REQUIRE(CheckBlockProofOfWork(header, params));
        }
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
    }
    auto* index{chainman.m_blockman.LookupBlockIndex(headers.front().GetHash())};
    BOOST_REQUIRE(index);
    CDiskBlockIndex disk{index};
    DataStream encoded;
    encoded << disk;
    CDiskBlockIndex loaded;
    encoded >> loaded;
    // Supply the reconstructed links and even the strongest disk status.
    // None of them is current-process proof evidence.
    loaded.phashBlock = index->phashBlock;
    loaded.pprev = index->pprev;
    loaded.nStatus = BLOCK_VALID_SCRIPTS;
    for (const CBlockIndex* untrusted : {static_cast<CBlockIndex*>(&disk), static_cast<CBlockIndex*>(&loaded)}) {
        InitYespowerVerificationCache(64);
        Observation observation;
        CacheVerifiedBlockIndexProof(*untrusted, params);
        BOOST_REQUIRE(CheckBlockProofOfWork(headers.front(), params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 1U);
#endif
    }
    {
        InitYespowerVerificationCache(64);
        Consensus::Params stricter{params};
        stricter.powLimit = uint256{1};
        Observation observation;
        CacheVerifiedBlockIndexProof(*index, stricter);
        BOOST_CHECK(!CheckBlockProofOfWork(headers.front(), stricter));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U); // Invalid target rejected before hashing.
#endif
    }
    {
        InitYespowerVerificationCache(64);
        index->nNonce ^= 1;
        Observation observation;
        CacheVerifiedBlockIndexProof(*index, params);
        BOOST_CHECK(!CheckBlockProofOfWork(index->GetBlockHeader(), params));
        index->nNonce ^= 1;
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 1U); // Altered header cannot inherit evidence.
#endif
    }
}

BOOST_FIXTURE_TEST_CASE(indexed_proof_does_not_accept_mutated_body_or_disk_header, TestingSetup)
{
    CacheBudget budget{DEFAULT_YESPOWER_CACHE_BYTES};
    auto& chainman{*m_node.chainman};
    // Same public height-1 fixture as the localhost P2P regression.
    DataStream data{ParseHex("00000020dc0a1b811b38833a4f9df704828c1dbccab7784c52faadfe995fb7dbc2ae5e7d79ec153afd87d19a5bf3d4b58737e26afe991832d7da6c45dfeb2f3343917541fc4e615dffff3f1f4d07000001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff03510101ffffffff02000000000100000023210267e719468109813a051f8e95a67fe3644fc99d3e6bcccfa00badd1776e58db58ac0000000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf90120000000000000000000000000000000000000000000000000000000000000000000000000")};
    auto block{std::make_shared<CBlock>()};
    data >> TX_WITH_WITNESS(*block);
    BOOST_REQUIRE(data.empty());
    const CBlockHeader header{*block};
    BlockValidationState state;
    {
        Observation observation;
        BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(std::span{&header, 1}, true, state, nullptr));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 1U); // The first proof is still mandatory.
#endif
    }
    auto bad{std::make_shared<CBlock>(*block)};
    CMutableTransaction tx{*bad->vtx.front()};
    tx.vin.front().scriptSig.push_back(0);
    bad->vtx.front() = MakeTransactionRef(tx);
    InitYespowerVerificationCache(64);
    {
        Observation observation;
        BOOST_CHECK(!chainman.ProcessNewBlock(bad, true, true, nullptr));
        BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, true, nullptr));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
    }
    LOCK(cs_main);
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), 1);
    auto* index{chainman.m_blockman.LookupBlockIndex(block->GetHash())};
    BOOST_REQUIRE(index);
    InitYespowerVerificationCache(64);
    {
        Observation observation;
        CBlock read;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(read, *index));
        BOOST_CHECK(read.GetHash() == block->GetHash());
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
    }
    CBlock corrupt{*block};
    corrupt.nNonce ^= 1;
    const auto saved{index->GetBlockPos()};
    const auto pos{chainman.m_blockman.WriteBlock(corrupt, 1)};
    index->nFile = pos.nFile;
    index->nDataPos = pos.nPos;
    InitYespowerVerificationCache(64);
    {
        Observation observation;
        CBlock read;
        BOOST_CHECK(!chainman.m_blockman.ReadBlock(read, *index));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 1U);
#endif
    }
    index->nFile = saved.nFile;
    index->nDataPos = saved.nPos;
}

BOOST_FIXTURE_TEST_CASE(indexed_proof_restore_is_safe_during_cache_resets, TestingSetup)
{
    CacheBudget budget{DEFAULT_YESPOWER_CACHE_BYTES};
    auto& chainman{*m_node.chainman};
    DataStream data{test::data::sugarchain_headers};
    CBlockHeader header;
    data >> header;
    BlockValidationState state;
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(std::span{&header, 1}, true, state, nullptr));
    const auto* index{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(header.GetHash()))};
    BOOST_REQUIRE(index);
    CBlockHeader invalid{header};
    invalid.nNonce ^= 1;
    std::atomic<unsigned> failures{0};
    {
        std::jthread resetter{[] {
            for (int i{0}; i < 1000; ++i) InitYespowerVerificationCache(64 + 32 * (i % 5));
        }};
        std::vector<std::jthread> readers;
        for (int n{0}; n < 4; ++n) {
            readers.emplace_back([&] {
                for (int i{0}; i < 25; ++i) {
                    {
                        LOCK(cs_main);
                        CacheVerifiedBlockIndexProof(*index, chainman.GetConsensus());
                    }
                    if (!CheckBlockProofOfWork(header, chainman.GetConsensus())) ++failures;
                    if (CheckBlockProofOfWork(invalid, chainman.GetConsensus())) ++failures;
                }
            });
        }
    }
    BOOST_CHECK_EQUAL(failures.load(), 0U);
}

BOOST_AUTO_TEST_CASE(mainnet_hashes_match_legacy_tls_bit_for_bit)
{
    DataStream stream{test::data::sugarchain_headers};
    std::vector<CBlockHeader> headers(6000);
    for (auto& header : headers) stream >> header;
    BOOST_REQUIRE(stream.empty());
    static constexpr uint8_t personal[]{"Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote"};
    const yespower_params_t official{YESPOWER_1_0, 2048, 32, personal, sizeof(personal) - 1};
    std::vector<uint256> expected, actual(headers.size());
    for (const auto& header : headers) {
        DataStream serialized;
        serialized << header;
        yespower_binary_t hash;
        BOOST_REQUIRE_EQUAL(yespower_tls(reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size(), &official, &hash), 0);
        expected.emplace_back(std::span{hash.uc});
    }
    std::atomic<size_t> next{0};
    {
        std::vector<std::jthread> workers;
        for (int i{0}; i < 8; ++i) {
            workers.emplace_back([&] {
                for (;;) {
                    const auto index{next.fetch_add(1)};
                    if (index >= headers.size()) break;
                    actual[index] = headers[index].GetPoWHash();
                }
            });
        }
    }
    for (size_t i{0}; i < headers.size(); ++i) BOOST_CHECK(actual[i] == expected[i]);

    // Lock down the invalid-nonce fixture used by the localhost P2P test.
    auto invalid{headers.front()};
    invalid.nNonce ^= 1;
    BOOST_CHECK(!CheckProofOfWorkImpl(invalid.GetPoWHash(), invalid.nBits, main->GetConsensus()));

    HeaderPoWVerifier verifier{8};
    Observation observation;
    BOOST_CHECK(verifier.Check(headers, main->GetConsensus()));
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_EQUAL(calls.load(), headers.size());
    BOOST_CHECK_LE(peak.load(), 8U);
#endif
}

BOOST_AUTO_TEST_CASE(cold_proofs_cache_reuse_and_worker_lifetime)
{
    unsigned tag{10};
    for (int workers : {1, 2, 8, 8, 1000, -1}) {
        const auto headers{Headers(tag++, 25)};
        HeaderPoWVerifier verifier{workers};
        BOOST_CHECK(verifier.Check({}, params));
        {
            Observation observation;
            BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), headers.size());
            BOOST_CHECK_LE(peak.load(), unsigned(std::clamp(workers, 1, MAX_HEADER_POW_WORKERS)));
            BOOST_CHECK_EQUAL(active.load(), 0U);
#endif
        }
        {
            Observation observation;
            BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
        }
        // Repeated construction/destruction also exercises scratch cleanup
        // under ASan/LSan rather than keeping all workers alive until exit.
    }
}

BOOST_AUTO_TEST_CASE(invalid_first_and_later_proofs_have_bounded_speculation)
{
    HeaderPoWVerifier verifier{8};
    unsigned tag{30};
    for (size_t position : {0U, 1U, 7U, 8U, 9U, 16U, 24U}) {
        auto headers{Headers(tag++, 25)};
        Invalidate(headers[position]);
        Observation observation;
        BOOST_CHECK(!verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        if (position == 0) {
            BOOST_CHECK_EQUAL(calls.load(), 1U);
        } else {
            // One serial admission check, then at most eight cold proofs per
            // decision. Invalid compact targets are not used as a shortcut.
            const auto upper{std::min(headers.size(), 1 + ((position + 7) / 8) * 8)};
            BOOST_CHECK_LE(calls.load(), upper);
            BOOST_CHECK_GE(calls.load(), 2U + ((position - 1) / 8) * 8);
        }
        BOOST_CHECK_LE(peak.load(), 8U);
        BOOST_CHECK_EQUAL(active.load(), 0U);
#endif
    }
    BOOST_CHECK(verifier.Check(Headers(50, 25), params));
}

BOOST_AUTO_TEST_CASE(concurrent_callers_share_one_bounded_pool)
{
    const auto first{Headers(60, 33)}, second{Headers(61, 33)};
    HeaderPoWVerifier verifier{8};
    std::atomic<bool> success{true};
    Observation observation;
    {
        std::jthread a{[&] { if (!verifier.Check(first, params)) success = false; }};
        std::jthread b{[&] { if (!verifier.Check(second, params)) success = false; }};
    }
    BOOST_CHECK(success.load());
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_EQUAL(calls.load(), first.size() + second.size());
    BOOST_CHECK_LE(peak.load(), 8U);
    BOOST_CHECK_EQUAL(active.load(), 0U);
#endif
}

BOOST_AUTO_TEST_CASE(current_target_and_algorithm_rules_are_not_cached)
{
    auto headers{Headers(70, 17)};
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, params));
    auto strict{main->GetConsensus()};
    BOOST_CHECK(!verifier.Check(headers, strict));
    auto bitcoin{params};
    bitcoin.fYespowerSugar = false;
    const bool expected{std::ranges::all_of(headers, [&](const auto& h) {
        return CheckProofOfWorkImpl(h.GetHash(), h.nBits, bitcoin);
    })};
    BOOST_CHECK_EQUAL(verifier.Check(headers, bitcoin), expected);
    for (uint32_t bits : {0U, 0x1f800001U, 0x23000001U}) {
        auto invalid{headers};
        invalid[8].nBits = bits;
        BOOST_CHECK(!verifier.Check(invalid, params));
    }
}

BOOST_AUTO_TEST_CASE(cache_budget_eviction_and_reset_only_trigger_reverification)
{
    const auto headers{Headers(90, 25)};
    CacheBudget budget{64}; // Two entries: force eviction without a large fixture.
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, params));
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_GE(calls.load(), headers.size() - 2);
#endif
    }
    InitYespowerVerificationCache(1 << 20);
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), headers.size());
#endif
    }
    BOOST_CHECK_THROW(InitYespowerVerificationCache(0), std::invalid_argument);
    BOOST_CHECK_THROW(InitYespowerVerificationCache(MAX_YESPOWER_CACHE_BYTES + 1), std::invalid_argument);
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U); // Rejected sizes leave existing evidence intact.
#endif
    }
}

BOOST_AUTO_TEST_CASE(cache_reset_is_safe_during_verification)
{
    const auto headers{Headers(91, 33)};
    auto invalid{headers.front()};
    Invalidate(invalid);
    CacheBudget budget{1 << 20};
    HeaderPoWVerifier verifier{8};
    std::atomic<bool> success{true};
    {
        std::jthread reset{[] {
            for (int i{0}; i < 40; ++i) InitYespowerVerificationCache(i % 2 ? 64 : 1 << 20);
        }};
        std::jthread reader{[&] {
            for (int i{0}; i < 4; ++i) {
                if (!verifier.Check(headers, params)) success = false;
                if (CheckBlockProofOfWork(invalid, params)) success = false;
            }
        }};
        for (int i{0}; i < 4; ++i) {
            if (!verifier.Check(headers, params)) success = false;
        }
    }
    BOOST_CHECK(success.load());
    BOOST_CHECK(!CheckBlockProofOfWork(invalid, params));
}

#ifdef ENABLE_YESPOWER_TEST_WRAP
BOOST_AUTO_TEST_CASE(local_worker_failure_reaches_caller_without_poisoning_queue)
{
    const auto headers{Headers(80, 17)};
    HeaderPoWVerifier verifier{8};
    {
        Observation observation{/*fail=*/2};
        BOOST_CHECK_THROW(verifier.Check(headers, params), std::runtime_error);
        BOOST_CHECK_LE(calls.load(), 9U);
        BOOST_CHECK_EQUAL(active.load(), 0U);
    }
    BOOST_CHECK(verifier.Check(headers, params));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
