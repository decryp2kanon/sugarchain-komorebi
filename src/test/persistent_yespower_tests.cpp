// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <dbwrapper.h>
#include <kernel/chainparams.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <streams.h>
#include <sync.h>
#include <test/data/sugarchain_headers.raw.h>
#include <test/util/setup_common.h>
#include <test/util/yespower_observer.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <vector>

namespace {
constexpr uint8_t DB_YESPOWER_EVIDENCE{'y'};
constexpr uint8_t DB_BLOCK_INDEX{'b'};
constexpr size_t CACHE_SIZE{8 << 20};

struct PersistentYespowerSetup : BasicTestingSetup {
    const std::unique_ptr<const CChainParams> main{CChainParams::Main()};
    const Consensus::Params& params{main->GetConsensus()};

    PersistentYespowerSetup() : BasicTestingSetup{ChainType::REGTEST} {}

    struct StoredChain {
        std::deque<uint256> hashes;
        std::deque<CBlockIndex> indexes;
        std::vector<const CBlockIndex*> pointers;

        void Add(const CBlockHeader& header)
        {
            hashes.push_back(header.GetHash());
            indexes.emplace_back(header);
            auto& index{indexes.back()};
            index.phashBlock = &hashes.back();
            index.pprev = indexes.size() > 1 ? &indexes[indexes.size() - 2] : nullptr;
            index.nHeight = indexes.size() - 1;
            index.nStatus = BLOCK_VALID_TREE;
            pointers.push_back(&index);
        }
    };

    static node::BlockMap Load(kernel::BlockTreeDB& db, const Consensus::Params& rules,
                               kernel::BlockTreeDB::YespowerEvidenceStats& stats, bool& success)
    {
        node::BlockMap result;
        const auto insert = [&](const uint256& hash) {
            if (hash.IsNull()) return static_cast<CBlockIndex*>(nullptr);
            const auto [it, inserted]{result.try_emplace(hash)};
            if (inserted) it->second.phashBlock = &it->first;
            return &it->second;
        };
        success = WITH_LOCK(cs_main, return db.LoadBlockIndexGuts(rules, insert, {}, &stats));
        return result;
    }

    void Write(kernel::BlockTreeDB& db, const StoredChain& chain) const
    {
        WITH_LOCK(cs_main, db.WriteBatchSync({}, 0, chain.pointers, params));
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(persistent_yespower_tests, PersistentYespowerSetup)

BOOST_AUTO_TEST_CASE(mainnet_fixture_persists_across_reopen)
{
    const fs::path path{m_args.GetDataDirBase() / "persistent_yespower_mainnet"};
    StoredChain chain;
    chain.Add(main->GenesisBlock());
    DataStream stream{test::data::sugarchain_headers};
    for (size_t i{0}; i < 6000; ++i) {
        CBlockHeader header;
        stream >> header;
        chain.Add(header);
    }

    kernel::BlockTreeDB::YespowerEvidenceStats first;
    const auto first_start{std::chrono::steady_clock::now()};
    {
        kernel::BlockTreeDB db{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true}};
        Write(db, chain);
        BOOST_CHECK(!db.Exists(std::make_pair(DB_YESPOWER_EVIDENCE, chain.hashes.front())));
        InitYespowerVerificationCache(1 << 20);
        bool success{false};
        test::YespowerCallCounter counter;
        const auto loaded{Load(db, params, first, success)};
        BOOST_REQUIRE(success);
        BOOST_CHECK_EQUAL(loaded.size(), chain.indexes.size());
        BOOST_CHECK_EQUAL(counter.Calls(), chain.indexes.size());
    }
    const auto first_time{std::chrono::duration<double>(std::chrono::steady_clock::now() - first_start).count()};
    BOOST_CHECK_EQUAL(first.hits, 0U);
    BOOST_CHECK_EQUAL(first.misses, chain.indexes.size());
    BOOST_CHECK_EQUAL(first.writes, chain.indexes.size());

    kernel::BlockTreeDB::YespowerEvidenceStats warm;
    const auto warm_start{std::chrono::steady_clock::now()};
    {
        kernel::BlockTreeDB db{{.path = path, .cache_bytes = CACHE_SIZE}};
        InitYespowerVerificationCache(1 << 20);
        bool success{false};
        test::YespowerCallCounter counter;
        const auto loaded{Load(db, params, warm, success)};
        BOOST_REQUIRE(success);
        BOOST_CHECK_EQUAL(loaded.size(), chain.indexes.size());
        BOOST_CHECK_EQUAL(counter.Calls(), 0U);
    }
    const auto warm_time{std::chrono::duration<double>(std::chrono::steady_clock::now() - warm_start).count()};
    BOOST_CHECK_EQUAL(warm.hits, chain.indexes.size());
    BOOST_CHECK_EQUAL(warm.misses, 0U);
    BOOST_CHECK_EQUAL(warm.writes, 0U);
    BOOST_TEST_MESSAGE("6001 real headers: first=" << first_time << "s warm=" << warm_time << "s");
}

BOOST_AUTO_TEST_CASE(evidence_is_versioned_bound_and_fail_safe)
{
    const fs::path path{m_args.GetDataDirBase() / "persistent_yespower_security"};
    StoredChain chain;
    chain.Add(main->GenesisBlock());
    const auto evidence_key{std::make_pair(DB_YESPOWER_EVIDENCE, chain.hashes.front())};
    const auto rules_hash{GetYespowerEvidenceRulesHash(params)};

    kernel::BlockTreeDB db{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true}};
    Write(db, chain);
    InitYespowerVerificationCache(1 << 20);
    kernel::BlockTreeDB::YespowerEvidenceStats initial;
    bool success{false};
    Load(db, params, initial, success);
    BOOST_REQUIRE(success);
    BOOST_CHECK_EQUAL(initial.misses, 1U);
    BOOST_CHECK(db.Exists(evidence_key));

    const auto check_changed_header = [&](CBlockHeader changed, unsigned expected_yespower_calls) {
        const uint256 changed_hash{changed.GetHash()};
        CBlockIndex changed_index{changed};
        changed_index.phashBlock = &changed_hash;
        changed_index.nStatus = BLOCK_VALID_TREE;
        WITH_LOCK(cs_main, db.Write(std::make_pair(DB_BLOCK_INDEX, chain.hashes.front()),
                                    CDiskBlockIndex{&changed_index}, true));
        InitYespowerVerificationCache(1 << 20);
        kernel::BlockTreeDB::YespowerEvidenceStats stats;
        test::YespowerCallCounter counter;
        Load(db, params, stats, success);
        BOOST_CHECK(!success);
        BOOST_CHECK_EQUAL(stats.hits, 0U);
        BOOST_CHECK_EQUAL(stats.misses, 1U);
        BOOST_CHECK_EQUAL(counter.Calls(), expected_yespower_calls);
        WITH_LOCK(cs_main, db.Write(std::make_pair(DB_BLOCK_INDEX, chain.hashes.front()),
                                    CDiskBlockIndex{chain.pointers.front()}, true));
    };

    // Evidence for the original serialized header cannot validate changed
    // header bytes, including a changed compact target.
    CBlockHeader changed_header{main->GenesisBlock()};
    changed_header.hashMerkleRoot = uint256::ONE;
    while (CheckBlockProofOfWork(changed_header, params)) ++changed_header.nNonce;
    check_changed_header(changed_header, 1);
    changed_header = CBlockHeader{main->GenesisBlock()};
    changed_header.nBits = 0;
    check_changed_header(changed_header, 0);

    // Unknown versions and malformed values cannot establish evidence. Both
    // cases fall back to a genuine proof and repair the entry.
    for (const bool malformed : {false, true}) {
        if (malformed) {
            db.Write(evidence_key, uint8_t{1});
        } else {
            db.Write(evidence_key, std::make_pair(uint8_t{99}, rules_hash));
        }
        InitYespowerVerificationCache(1 << 20);
        kernel::BlockTreeDB::YespowerEvidenceStats stats;
        Load(db, params, stats, success);
        BOOST_REQUIRE(success);
        BOOST_CHECK_EQUAL(stats.hits, 0U);
        BOOST_CHECK_EQUAL(stats.misses, 1U);
        BOOST_CHECK_EQUAL(stats.writes, 1U);
    }

    // A changed rules identity cannot reuse otherwise valid evidence.
    auto other_network{params};
    other_network.hashGenesisBlock = uint256::ONE;
    InitYespowerVerificationCache(1 << 20);
    kernel::BlockTreeDB::YespowerEvidenceStats network_stats;
    Load(db, other_network, network_stats, success);
    BOOST_REQUIRE(success);
    BOOST_CHECK_EQUAL(network_stats.hits, 0U);
    BOOST_CHECK_EQUAL(network_stats.misses, 1U);

    // Returning to the original rules also misses after the other ruleset
    // replaced the value; evidence never crosses rulesets in either direction.
    InitYespowerVerificationCache(1 << 20);
    kernel::BlockTreeDB::YespowerEvidenceStats restored_stats;
    Load(db, params, restored_stats, success);
    BOOST_REQUIRE(success);
    BOOST_CHECK_EQUAL(restored_stats.hits, 0U);
    BOOST_CHECK_EQUAL(restored_stats.misses, 1U);

    auto stricter{params};
    stricter.powLimit = uint256::ONE;
    InitYespowerVerificationCache(1 << 20);
    kernel::BlockTreeDB::YespowerEvidenceStats strict_stats;
    Load(db, stricter, strict_stats, success);
    BOOST_CHECK(!success);
    BOOST_CHECK_EQUAL(strict_stats.hits, 0U);
    BOOST_CHECK_EQUAL(strict_stats.misses, 1U);

    // Full -reindex wipes the block-tree database, including this key-space.
    {
        kernel::BlockTreeDB wiped{{.path = m_args.GetDataDirBase() / "persistent_yespower_reindex",
                                   .cache_bytes = CACHE_SIZE, .wipe_data = true}};
        wiped.Write(evidence_key, std::make_pair(uint8_t{1}, rules_hash));
    }
    {
        kernel::BlockTreeDB wiped{{.path = m_args.GetDataDirBase() / "persistent_yespower_reindex",
                                   .cache_bytes = CACHE_SIZE, .wipe_data = true}};
        BOOST_CHECK(!wiped.Exists(evidence_key));
    }
}

BOOST_AUTO_TEST_CASE(invalid_header_cannot_create_evidence)
{
    const fs::path path{m_args.GetDataDirBase() / "persistent_yespower_invalid"};
    CBlockHeader invalid{main->GenesisBlock()};
    invalid.nBits = 0;
    StoredChain chain;
    chain.Add(invalid);
    const auto evidence_key{std::make_pair(DB_YESPOWER_EVIDENCE, chain.hashes.front())};

    kernel::BlockTreeDB db{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true}};
    Write(db, chain);
    InitYespowerVerificationCache(1 << 20);
    kernel::BlockTreeDB::YespowerEvidenceStats stats;
    bool success{true};
    Load(db, params, stats, success);
    BOOST_CHECK(!success);
    BOOST_CHECK_EQUAL(stats.hits, 0U);
    BOOST_CHECK_EQUAL(stats.misses, 1U);
    BOOST_CHECK_EQUAL(stats.writes, 0U);
    BOOST_CHECK(!db.Exists(evidence_key));
}

BOOST_AUTO_TEST_SUITE_END()
