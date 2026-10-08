// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <crypto/yespower-1.0.1/yespower.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/interface_ui.h>
#include <node/startup_progress.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/data/sugarchain_headers.raw.h>
#include <test/util/setup_common.h>
#include <test/util/net.h>
#include <test/util/logging.h>
#include <tinyformat.h>
#include <node/protocol_version.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <boost/signals2/connection.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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

    bool LoadStartupHeaders(std::vector<CBlockHeader> headers, int workers, bool cancel_before = false,
                            bool fast_startup = false, uint32_t disk_status = BLOCK_VALID_UNKNOWN,
                            bool cache_loaded = false, std::span<const uint32_t> disk_statuses = {})
    {
        BOOST_REQUIRE(disk_statuses.empty() || disk_statuses.size() == headers.size());
        kernel::BlockTreeDB db{DBParams{.path = "", .cache_bytes = 1 << 20, .memory_only = true}};
        std::vector<uint256> hashes;
        std::vector<std::unique_ptr<CBlockIndex>> indexes;
        std::vector<const CBlockIndex*> entries;
        hashes.reserve(headers.size());
        indexes.reserve(headers.size());
        entries.reserve(headers.size());
        for (size_t i{0}; i < headers.size(); ++i) {
            hashes.push_back(headers[i].GetHash());
            indexes.push_back(std::make_unique<CBlockIndex>(headers[i]));
            indexes.back()->phashBlock = &hashes.back();
            indexes.back()->nHeight = i;
            indexes.back()->nStatus = disk_statuses.empty() ? disk_status : disk_statuses[i];
            entries.push_back(indexes.back().get());
        }
        db.WriteBatchSync({}, 0, entries);
        node::BlockMap loaded;
        const auto insert = [&](const uint256& hash) -> CBlockIndex* {
            if (hash.IsNull()) return nullptr;
            auto [it, inserted] = loaded.try_emplace(hash);
            if (inserted) it->second.phashBlock = &it->first;
            return &it->second;
        };
        util::SignalInterrupt cancelled;
        if (cancel_before) (void)cancelled();
        const auto& interrupt{cancel_before ? cancelled : m_interrupt};
        unsigned reserve_calls{0};
        const auto reserve = [&](uint64_t count) {
            BOOST_CHECK(loaded.empty());
            BOOST_CHECK_EQUAL(count, headers.size());
            ++reserve_calls;
            loaded.reserve(count);
        };
        const bool valid{WITH_LOCK(cs_main, return db.LoadBlockIndexGuts(params, insert, interrupt, workers, fast_startup, reserve))};
        BOOST_CHECK_EQUAL(reserve_calls, cancel_before || !fast_startup ? 0U : 1U);
        if (valid) {
            BOOST_CHECK_EQUAL(loaded.size(), headers.size());
            for (auto& [hash, index] : loaded) {
                if (disk_statuses.empty()) BOOST_CHECK_EQUAL(index.nStatus, disk_status);
                if (cache_loaded) WITH_LOCK(cs_main, CacheVerifiedBlockIndexProof(index, params));
            }
        }
        return valid;
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

BOOST_FIXTURE_TEST_CASE(indexed_header_messages_after_cache_eviction, TestingSetup)
{
    CacheBudget budget{DEFAULT_YESPOWER_CACHE_BYTES};
    auto& chainman{*m_node.chainman};
    DataStream stream{test::data::sugarchain_headers};
    std::vector<CBlockHeader> headers(2000);
    for (auto& header : headers) stream >> header;
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, chainman.GetConsensus()));
    BlockValidationState state;
    // This fixture isolates an already admitted index. Separate P2P tests
    // retain the actual mainnet minimum-chainwork admission requirement.
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(headers, true, state, nullptr));
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CNode peer{0, nullptr, CAddress{}, 0, 0, CService{}, "", ConnectionType::INBOUND, false, 0};
    auto& connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    connman.Handshake(peer, true, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                     NODE_NETWORK, PROTOCOL_VERSION, true);
    BOOST_REQUIRE(peer.fSuccessfullyConnected);
    auto receive = [&](std::span<const CBlockHeader> batch, [[maybe_unused]] unsigned expected_calls) {
        connman.FlushSendBuffer(peer);
        auto message{NetMsg::Make(NetMsgType::HEADERS)};
        VectorWriter writer{message.data, 0};
        WriteCompactSize(writer, batch.size());
        for (const auto& header : batch) writer << header << uint8_t{0};
        Observation observation;
        const auto start{std::chrono::steady_clock::now()};
        BOOST_REQUIRE(connman.ReceiveMsgFrom(peer, std::move(message)));
        peer.fPauseSend = false;
        connman.ProcessMessagesOnce(peer);
        BOOST_TEST_MESSAGE("indexed HEADERS seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                           << " yespower_calls=" << calls.load());
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), expected_calls);
#endif
    };
    InitYespowerVerificationCache(DEFAULT_YESPOWER_CACHE_BYTES);
    receive(headers, 0); // Known headers with an empty bounded cache.
    receive(headers, 0); // Ordinary warm-cache control.
    BOOST_CHECK(!peer.fDisconnect);

    CBlockHeader next;
    stream >> next;
    std::array<CBlockHeader, 2> mixed{headers.back(), next};
    InitYespowerVerificationCache(DEFAULT_YESPOWER_CACHE_BYTES);
    receive(mixed, 1); // An unknown header still requires its first real proof.
    BOOST_CHECK(!peer.fDisconnect);
    {
        LOCK(cs_main);
        // The short chain still has not met mainnet's admission threshold.
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(next.GetHash()));
    }

    auto invalid{headers.front()};
    do { ++invalid.nNonce; } while (CheckProofOfWorkImpl(invalid.GetPoWHash(), invalid.nBits, chainman.GetConsensus()));
    {
        ASSERT_DEBUG_LOG("header with invalid proof of work");
        receive(std::span{&invalid, 1}, 1); // A changed header cannot inherit evidence.
    }
    m_node.peerman->FinalizeNode(peer);
}

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

BOOST_AUTO_TEST_CASE(startup_checks_each_header_with_configured_workers)
{
    const auto headers{Headers(301, 25)};
    for (const int workers : {1, 2, 4, 8}) {
        CacheBudget budget{1 << 20};
        HeaderPoWVerifier verifier{workers};
        {
            Observation observation;
            BOOST_REQUIRE(verifier.CheckEach(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), headers.size());
            BOOST_CHECK_LE(peak.load(), static_cast<unsigned>(workers));
#endif
        }
        {
            Observation observation;
            BOOST_CHECK(verifier.CheckEach(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), 0U); // Every check still consults verified process-local evidence.
#endif
        }
        auto invalid{headers};
        Invalidate(invalid[12]);
        BOOST_CHECK(!verifier.CheckEach(invalid, params));
    }
}

BOOST_AUTO_TEST_CASE(startup_block_index_load_checks_every_header)
{
    const auto valid{Headers(302, 2001)};
    BOOST_CHECK(LoadStartupHeaders(valid, 1));
    BOOST_CHECK(LoadStartupHeaders(valid, 3));
    BOOST_CHECK(LoadStartupHeaders(valid, 8));
    BOOST_CHECK(!LoadStartupHeaders(valid, 8, true));

    auto invalid{Headers(303, 17)};
    Invalidate(invalid[8]);
    BOOST_CHECK(!LoadStartupHeaders(invalid, 1));
    BOOST_CHECK(!LoadStartupHeaders(invalid, 8));
}

BOOST_AUTO_TEST_CASE(fast_startup_trusts_only_eligible_disk_headers)
{
    const auto valid{Headers(304, 4)};
    for (const int workers : {1, 8}) {
        CacheBudget budget{1 << 20};
        {
            Observation observation;
            BOOST_CHECK(LoadStartupHeaders(valid, workers, false, false, BLOCK_VALID_TREE));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), valid.size());
#endif
        }
        InitYespowerVerificationCache(1 << 20);
        {
            Observation observation;
            BOOST_CHECK(LoadStartupHeaders(valid, workers, false, true, BLOCK_VALID_TREE));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
        }
        InitYespowerVerificationCache(1 << 20);
        {
            Observation observation;
            BOOST_CHECK(LoadStartupHeaders(valid, workers, false, true, BLOCK_VALID_UNKNOWN));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), valid.size());
#endif
        }
        InitYespowerVerificationCache(1 << 20);
        {
            Observation observation;
            BOOST_CHECK(LoadStartupHeaders(valid, workers, false, true, BLOCK_VALID_TREE | BLOCK_FAILED_VALID));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), valid.size());
#endif
        }
        InitYespowerVerificationCache(1 << 20);
        {
            Observation observation;
            BOOST_CHECK(LoadStartupHeaders(valid, workers, false, true, BLOCK_VALID_TREE | BLOCK_FAILED_CHILD));
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), valid.size());
#endif
        }
    }

    auto invalid{valid};
    Invalidate(invalid[0]);
    InitYespowerVerificationCache(1 << 20);
    BOOST_CHECK(!LoadStartupHeaders(invalid, 8, false, false, BLOCK_VALID_TREE));
    // This acceptance is the explicit opt-in disk-trust trade-off.
    BOOST_CHECK(LoadStartupHeaders(invalid, 8, false, true, BLOCK_VALID_TREE));
    BOOST_CHECK(!LoadStartupHeaders(invalid, 8, false, true, BLOCK_VALID_UNKNOWN));
    invalid[0].nBits = 0;
    BOOST_CHECK(!LoadStartupHeaders(invalid, 8, false, true, BLOCK_VALID_TREE));
    BOOST_CHECK(!LoadStartupHeaders(valid, 8, true, true, BLOCK_VALID_TREE));
}

BOOST_AUTO_TEST_CASE(fast_startup_never_promotes_disk_trust_to_live_proof_cache)
{
    CacheBudget budget{1 << 20};
    const auto header{Headers(305, 1)};
    BOOST_REQUIRE(LoadStartupHeaders(header, 8, false, true, BLOCK_VALID_TREE, true));
    {
        Observation observation;
        BOOST_CHECK(CheckBlockProofOfWork(header.front(), params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 1U);
#endif
    }

    // Later network batches still validate previously unseen headers.
    HeaderPoWVerifier verifier{8};
    const auto fresh{Headers(307, 2)};
    {
        Observation observation;
        BOOST_CHECK(verifier.Check(fresh, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), fresh.size());
#endif
    }
    auto invalid{Headers(308, 1)};
    Invalidate(invalid.front());
    BOOST_CHECK(!verifier.Check(invalid, params));
}

BOOST_AUTO_TEST_CASE(fast_startup_flushes_untrusted_headers_before_trusted_entries)
{
    CacheBudget budget{1 << 20};
    const auto headers{Headers(306, 4)};
    const std::array<uint32_t, 4> statuses{BLOCK_VALID_TREE, BLOCK_VALID_UNKNOWN,
                                            BLOCK_VALID_TREE, BLOCK_VALID_UNKNOWN};
    Observation observation;
    BOOST_CHECK(LoadStartupHeaders(headers, 8, false, true, BLOCK_VALID_UNKNOWN, false, statuses));
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_EQUAL(calls.load(), 2U);
#endif
}

BOOST_AUTO_TEST_CASE(fast_startup_still_checks_genesis)
{
    CacheBudget budget{1 << 20};
    Observation observation;
    BOOST_CHECK(LoadStartupHeaders({main->GenesisBlock()}, 8, false, true, BLOCK_VALID_TREE));
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_EQUAL(calls.load(), 1U);
#endif
}

BOOST_AUTO_TEST_CASE(startup_progress_counts_entries_and_reports_one_percent_buckets)
{
    const auto headers{Headers(309, 101)};
    for (const bool fast_startup : {false, true}) {
        std::vector<std::string> messages;
        boost::signals2::scoped_connection capture{uiInterface.InitMessage_connect(
            [&](const std::string& message) { messages.push_back(message); })};
        CacheBudget budget{1 << 20};
        Observation observation;
        BOOST_CHECK(LoadStartupHeaders(headers, 8, false, fast_startup, BLOCK_VALID_TREE));
        if (!fast_startup) {
            BOOST_REQUIRE_EQUAL(messages.size(), 1U);
            BOOST_CHECK(messages.front().find("Loading block index: 101 | ") == 0);
            BOOST_CHECK(messages.front().find("ETA") == std::string::npos);
#ifdef ENABLE_YESPOWER_TEST_WRAP
            BOOST_CHECK_EQUAL(calls.load(), headers.size());
#endif
            continue;
        }
        BOOST_REQUIRE_EQUAL(messages.size(), 101U);
        BOOST_CHECK_EQUAL(messages.front(), "Counting block index entries...");
        for (unsigned percent{1}; percent <= 100; ++percent) {
            const auto& message{messages[percent]};
            BOOST_CHECK(message.find("Loading block index: ") == 0);
            BOOST_CHECK(message.find(" / 101 (") != std::string::npos);
            BOOST_CHECK(message.find(strprintf("(%u%%)", percent)) != std::string::npos);
            BOOST_CHECK(message.find("/s | elapsed ") != std::string::npos);
            BOOST_CHECK(message.find(" | ETA ") != std::string::npos);
        }
        BOOST_CHECK(messages.back().find("101 / 101 (100%)") != std::string::npos);
        BOOST_CHECK(messages.back().find(" | ETA 0s") != std::string::npos);
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), fast_startup ? 0U : headers.size());
#endif
    }
}

BOOST_AUTO_TEST_CASE(startup_progress_handles_empty_small_and_interrupted_indexes)
{
    std::vector<std::string> messages;
    boost::signals2::scoped_connection capture{uiInterface.InitMessage_connect(
        [&](const std::string& message) { messages.push_back(message); })};
    BOOST_CHECK(LoadStartupHeaders({}, 1, false, true));
    BOOST_REQUIRE_EQUAL(messages.size(), 2U);
    BOOST_CHECK(messages.back().find("0 / 0 (100%)") != std::string::npos);
    BOOST_CHECK(messages.back().find(" | ETA 0s") != std::string::npos);

    messages.clear();
    BOOST_CHECK(LoadStartupHeaders(Headers(310, 3), 8, false, true));
    BOOST_REQUIRE_EQUAL(messages.size(), 4U);
    BOOST_CHECK(messages[1].find("(33%)") != std::string::npos);
    BOOST_CHECK(messages[2].find("(66%)") != std::string::npos);
    BOOST_CHECK(messages[3].find("3 / 3 (100%)") != std::string::npos);

    messages.clear();
    const auto interrupted_headers{Headers(311, 3)};
    Observation observation;
    BOOST_CHECK(!LoadStartupHeaders(interrupted_headers, 8, true, true));
    BOOST_REQUIRE_EQUAL(messages.size(), 1U);
    BOOST_CHECK_EQUAL(messages.front(), "Counting block index entries...");
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_EQUAL(calls.load(), 0U);
#endif
}

BOOST_AUTO_TEST_CASE(startup_legacy_progress_retains_2000_entry_batches)
{
    const auto headers{Headers(312, 2001)};
    for (const int workers : {1, 3, 8}) {
        std::vector<std::string> messages;
        boost::signals2::scoped_connection capture{uiInterface.InitMessage_connect(
            [&](const std::string& message) { messages.push_back(message); })};
        CacheBudget budget{1 << 20};
        BOOST_REQUIRE(LoadStartupHeaders(headers, workers));
        BOOST_REQUIRE_EQUAL(messages.size(), 2U);
        BOOST_CHECK(messages[0].find("Loading block index: 2,000 | ") == 0);
        BOOST_CHECK(messages[1].find("Loading block index: 2,001 | ") == 0);
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

BOOST_AUTO_TEST_CASE(batch_evidence_survives_individual_eviction_and_binds_order_and_count)
{
    // The individual table cannot hold these 65 inputs; the separate table
    // retains their complete, successfully checked batch within the same KiB.
    CacheBudget budget{1024};
    const auto headers{Headers(120, 65)};
    HeaderPoWVerifier verifier{8};
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), headers.size());
#endif
    }
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_LE(calls.load(), 1U); // First-header admission is retained.
#endif
    }
    {
        Observation observation;
        BOOST_REQUIRE(CheckBlockProofOfWork(headers.back(), params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), 0U); // A hit refills ordinary evidence.
#endif
    }
    auto reordered{headers};
    std::reverse(reordered.begin(), reordered.end());
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(reordered, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_GT(calls.load(), 1U); // Different order must miss the batch.
#endif
    }
    reordered.pop_back();
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(reordered, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_GT(calls.load(), 1U); // Different count is a different key.
#endif
    }
    InitYespowerVerificationCache(1024);
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_EQUAL(calls.load(), headers.size());
#endif
    }
    // One-worker callers can use the same successful evidence, without
    // creating any additional parallel workers.
    HeaderPoWVerifier serial{1};
    Observation observation;
    BOOST_REQUIRE(serial.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_LE(calls.load(), 1U);
#endif
}

BOOST_AUTO_TEST_CASE(batch_evidence_never_authorizes_mutations_or_stricter_later_targets)
{
    CacheBudget budget{1024};
    auto headers{Headers(121, 33)};
    // This first header satisfies mainnet's tighter target; later easy proofs
    // do not. A first-header-only limit check would incorrectly authorize them.
    headers.front() = main->GenesisBlock();
    BOOST_REQUIRE(CheckProofOfWorkImpl(headers.front().GetPoWHash(), headers.front().nBits, main->GetConsensus()));
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, params));
    BOOST_CHECK(!verifier.Check(headers, main->GetConsensus()));
    auto invalid{headers};
    Invalidate(invalid.back());
    BOOST_CHECK(!verifier.Check(invalid, params));
    BOOST_CHECK(!verifier.Check(invalid, params)); // Failure must not be cached.
    invalid = headers;
    invalid.back().nBits = 0;
    BOOST_CHECK(!verifier.Check(invalid, params));
    const std::array<void (*)(CBlockHeader&), 5> mutate{
        [](CBlockHeader& h) { ++h.nVersion; },
        [](CBlockHeader& h) { ++h.hashPrevBlock.begin()[0]; },
        [](CBlockHeader& h) { ++h.hashMerkleRoot.begin()[0]; },
        [](CBlockHeader& h) { ++h.nTime; },
        [](CBlockHeader& h) { --h.nBits; },
    };
    for (const auto change : mutate) {
        invalid = headers;
        unsigned attempts{0};
        do {
            BOOST_REQUIRE_LT(++attempts, 1000U);
            change(invalid.back());
        } while (CheckProofOfWorkImpl(invalid.back().GetPoWHash(), invalid.back().nBits, params));
        BOOST_REQUIRE(DeriveTarget(invalid.back().nBits, params.powLimit));
        BOOST_CHECK(!verifier.Check(invalid, params));
    }
    BOOST_REQUIRE(verifier.Check(headers, params));
}

BOOST_AUTO_TEST_CASE(batch_evidence_is_safe_during_concurrent_cache_resets)
{
    // Both budgets enable batch evidence, but cannot retain all individual
    // proofs. Exercise RestoreBatch concurrently with table replacement.
    CacheBudget budget{1024};
    auto headers{Headers(122, 65)};
    headers.front() = main->GenesisBlock();
    auto invalid{headers};
    Invalidate(invalid.back());
    HeaderPoWVerifier verifier{8};
    BOOST_REQUIRE(verifier.Check(headers, params));
    {
        Observation observation;
        BOOST_REQUIRE(verifier.Check(headers, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
        BOOST_CHECK_LE(calls.load(), 1U); // Confirm the intended batch-hit path.
#endif
    }
    std::atomic<bool> start{false}, success{true};
    Observation observation;
    auto reader = [&] {
        while (!start.load()) std::this_thread::yield();
        for (int i{0}; i < 8; ++i) {
            if (!verifier.Check(headers, params)) success = false;
            if (verifier.Check(invalid, params)) success = false;
            // The first proof meets this limit; later easy-target proofs do
            // not. Reset/reuse must never bypass per-header target admission.
            if (verifier.Check(headers, main->GetConsensus())) success = false;
        }
    };
    {
        std::jthread a{reader}, b{reader};
        std::jthread resetter{[&] {
            start = true;
            for (int i{0}; i < 40; ++i) {
                InitYespowerVerificationCache(i % 2 ? 1024 : 2048);
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
    }
    BOOST_CHECK(success.load());
    BOOST_CHECK(verifier.Check(headers, params));
    BOOST_CHECK(!verifier.Check(invalid, params));
#ifdef ENABLE_YESPOWER_TEST_WRAP
    BOOST_CHECK_LE(peak.load(), 8U);
    BOOST_CHECK_EQUAL(active.load(), 0U);
#endif
}

BOOST_AUTO_TEST_CASE(mainnet_batch_evidence_after_individual_cache_pressure)
{
    CacheBudget budget{128 << 10};
    DataStream stream{test::data::sugarchain_headers};
    std::vector<CBlockHeader> headers(6000);
    for (auto& header : headers) stream >> header;
    BOOST_REQUIRE(stream.empty());
    HeaderPoWVerifier verifier{8};
    const auto& consensus{main->GetConsensus()};
    for (bool repeat : {false, true}) {
        Observation observation;
        for (size_t offset{0}; offset < headers.size(); offset += 2000) {
            BOOST_REQUIRE(verifier.Check(std::span{headers}.subspan(offset, 2000), consensus));
        }
#ifdef ENABLE_YESPOWER_TEST_WRAP
        if (repeat) {
            BOOST_CHECK_LE(calls.load(), 3U);
        } else {
            BOOST_CHECK_EQUAL(calls.load(), headers.size());
        }
#endif
    }
    // Changing a real header cannot inherit the successful batch's evidence.
    auto& changed{headers[1500]};
    do { ++changed.nNonce; } while (CheckProofOfWorkImpl(changed.GetPoWHash(), changed.nBits, consensus));
    BOOST_CHECK(!verifier.Check(std::span{headers}.first(2000), consensus));
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
    {
        // A partial/failed batch must not become successful batch evidence.
        // Fail the next remaining cold proof on the retry as well.
        Observation observation{/*fail=*/1};
        BOOST_CHECK_THROW(verifier.Check(headers, params), std::runtime_error);
        BOOST_CHECK_GT(calls.load(), 0U);
        BOOST_CHECK_EQUAL(active.load(), 0U);
    }
    BOOST_CHECK(verifier.Check(headers, params));
}
#endif

BOOST_AUTO_TEST_CASE(startup_phase_progress_reports_real_work_and_opaque_sort)
{
    std::vector<std::string> messages;
    boost::signals2::scoped_connection connection{uiInterface.InitMessage_connect(
        [&](const std::string& message) { messages.push_back(message); })};
    {
        node::StartupProgress progress{"Linking block index", 10000};
        for (unsigned i = 0; i < 10000; ++i) progress.Advance();
        BOOST_REQUIRE_EQUAL(messages.size(), 100U); // Start plus 1..99, not premature 100.
        progress.Finish();
    }
    BOOST_REQUIRE_EQUAL(messages.size(), 101U);
    for (unsigned percent = 1; percent <= 100; ++percent) {
        BOOST_CHECK(messages[percent].find(strprintf("(%u%%)", percent)) != std::string::npos);
    }
    BOOST_CHECK(messages.back().find("10,000 / 10,000 (100%)") != std::string::npos);
    messages.clear();
    {
        node::StartupProgress progress{"Checking block files", 3};
        progress.Advance();
        progress.Finish(); // Incomplete work cannot publish 100.
    }
    BOOST_REQUIRE_EQUAL(messages.size(), 2U);
    BOOST_CHECK(messages.back().find("(33%)") != std::string::npos);
    messages.clear();
    {
        node::StartupProgress sorting{"Sorting block headers", 44782474};
        sorting.FinishSort();
    }
    BOOST_REQUIRE_EQUAL(messages.size(), 2U);
    BOOST_CHECK(messages.front().find("44,782,474 entries") != std::string::npos);
    for (const auto& message : messages) {
        BOOST_CHECK(message.find('%') == std::string::npos);
        BOOST_CHECK(message.find("ETA") == std::string::npos);
    }
    messages.clear();
    node::StartupProgress empty{"Linking block index", 0};
    empty.Finish();
    BOOST_CHECK(messages.back().find("0 / 0 (100%)") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
