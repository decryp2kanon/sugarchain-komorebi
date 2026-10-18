// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <undo.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

using kernel::ChainstateRole;

// Taken from validation.cpp
static constexpr auto DATABASE_WRITE_INTERVAL_MIN{50min};
static constexpr auto DATABASE_WRITE_INTERVAL_MAX{70min};

BOOST_AUTO_TEST_SUITE(chainstate_write_tests)

BOOST_FIXTURE_TEST_CASE(chainstate_write_interval, TestingSetup)
{
    struct TestSubscriber final : CValidationInterface {
        bool m_did_flush{false};
        void ChainStateFlushed(const ChainstateRole&, const CBlockLocator&) override
        {
            m_did_flush = true;
        }
    };

    const auto sub{std::make_shared<TestSubscriber>()};
    m_node.validation_signals->RegisterSharedValidationInterface(sub);
    auto& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    BlockValidationState state_dummy{};

    // The first periodic flush sets m_next_write and does not flush
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!sub->m_did_flush);

    // The periodic flush interval is between 50 and 70 minutes (inclusive)
    SetMockTime(GetTime<std::chrono::minutes>() + DATABASE_WRITE_INTERVAL_MIN - 1min);
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!sub->m_did_flush);

    SetMockTime(GetTime<std::chrono::minutes>() + DATABASE_WRITE_INTERVAL_MAX);
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(sub->m_did_flush);
}

struct DiskChain100Setup : TestChain100Setup {
    DiskChain100Setup() : TestChain100Setup{ChainType::REGTEST, {
        .coins_db_in_memory = false,
        .block_tree_db_in_memory = false,
        .setup_net = false,
    }} {}
};

// Test that we do PERIODIC flushes inside ActivateBestChain.
// This is necessary for reindex-chainstate to be able to periodically flush
// before reaching chain tip.
BOOST_FIXTURE_TEST_CASE(write_during_multiblock_activation, DiskChain100Setup)
{
    struct TestSubscriber final : CValidationInterface
    {
        const CBlockIndex* m_tip{nullptr};
        const CBlockIndex* m_flushed_at_block{nullptr};
        void ChainStateFlushed(const ChainstateRole&, const CBlockLocator&) override
        {
            m_flushed_at_block = m_tip;
        }
        void UpdatedBlockTip(const CBlockIndex* block_index, const CBlockIndex*, bool) override {
            m_tip = block_index;
        }
    };

    auto& node{m_node};
    auto& chainman{*Assert(node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    auto& blockman{chainman.m_blockman};
    BlockValidationState state_dummy{};
    // This fixture exercises strict startup without historical disk-PoW trust.
    BOOST_CHECK(!blockman.UsesHistoricalPoWTrust());

    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    CBlockIndex* second_from_tip{tip->pprev};
    const auto disconnect_two = [&] {
        LOCK2(chainman.GetMutex(), chainstate.MempoolMutex());
        BOOST_REQUIRE(chainstate.DisconnectTip(state_dummy, nullptr));
        BOOST_REQUIRE(chainstate.DisconnectTip(state_dummy, nullptr));
        BOOST_CHECK_EQUAL(second_from_tip->pprev, chainstate.m_chain.Tip());
    };
    disconnect_two();

    // Set the next write deadline, then advance beyond the periodic interval.
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state_dummy, FlushStateMode::FORCE_FLUSH));
    node.validation_signals->SyncWithValidationInterfaceQueue();
    SetMockTime(GetMockTime() + DATABASE_WRITE_INTERVAL_MAX);

    const auto sub{std::make_shared<TestSubscriber>()};
    node.validation_signals->RegisterSharedValidationInterface(sub);

    BOOST_REQUIRE(chainstate.ActivateBestChain(state_dummy, nullptr));
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip());
    // PERIODIC runs within the outer activation loop, before reaching tip.
    node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(sub->m_flushed_at_block, second_from_tip);

    disconnect_two();
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state_dummy, FlushStateMode::FORCE_FLUSH));
    node.validation_signals->SyncWithValidationInterfaceQueue();
    sub->m_tip = nullptr;
    sub->m_flushed_at_block = nullptr;
    const auto unchanged_time{GetMockTime()};

    // Preload dirty side indexes directly so header acceptance cannot flush
    // them for us. Each is a genuinely mined regtest child of genesis.
    // Only the real blocks above are activated; consensus checks stay enabled.
    std::vector<uint256> side_hashes;
    side_hashes.reserve(node::BlockManager::MAX_DIRTY_BLOCK_INDEX + 1);
    {
        LOCK(cs_main);
        CBlockHeader header{chainman.GetParams().GenesisBlock()};
        header.hashPrevBlock = chainstate.m_chain.Genesis()->GetBlockHash();
        ++header.nTime;
        CBlockIndex* best_header{chainstate.m_chain.Tip()};
        const auto add_side_header = [&] {
            do { ++header.nNonce; } while (!CheckBlockProofOfWork(header, chainman.GetConsensus()));
            const auto* index{blockman.AddToBlockIndex(header, best_header)};
            BOOST_REQUIRE_EQUAL(index->nHeight, 1);
            BOOST_CHECK(!index->HasCurrentProcessYespowerEvidence());
            side_hashes.push_back(index->GetBlockHash());
        };
        for (size_t i = 0; i < node::BlockManager::MAX_DIRTY_BLOCK_INDEX - 1; ++i) {
            add_side_header();
        }
        BOOST_CHECK(!blockman.BlockIndexWriteDue());
        BOOST_REQUIRE(chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC));
        CDiskBlockIndex disk;
        BOOST_CHECK(!blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, side_hashes.front()), disk));

        add_side_header();
        BOOST_CHECK(blockman.BlockIndexWriteDue());
        add_side_header(); // Also handle an activation batch overshooting the threshold.
        for (const auto mode : {FlushStateMode::NONE, FlushStateMode::IF_NEEDED}) {
            BOOST_REQUIRE(chainstate.FlushStateToDisk(state_dummy, mode));
            BOOST_CHECK(blockman.BlockIndexWriteDue());
            BOOST_CHECK(!blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, side_hashes.front()), disk));
        }
        BOOST_CHECK(chainstate.GetCoinsCacheSizeState() == CoinsCacheSizeState::OK);
    }
    node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(sub->m_flushed_at_block == nullptr);

    // With no timer or coin-cache pressure, ordinary multi-block activation
    // must still persist the dirty indexes through its PERIODIC call.
    BOOST_REQUIRE(chainstate.ActivateBestChain(state_dummy, nullptr));
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip());
    node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(sub->m_flushed_at_block, second_from_tip);
    BOOST_CHECK(GetMockTime() == unchanged_time);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(!blockman.BlockIndexWriteDue());
        BOOST_CHECK(chainstate.CoinsDB().GetBestBlock() == second_from_tip->GetBlockHash());
        // The threshold sync keeps reusable coins cached. A FORCE_FLUSH
        // substitution would discard this coin before the next block.
        BOOST_CHECK(chainstate.CoinsTip().HaveCoinInCache(COutPoint{
            m_coinbase_txns[second_from_tip->nHeight - 1]->GetHash(), 0}));

        // Reopen the real DB without another flush: all pressure entries
        // and referenced block/undo metadata must already be durable.
        const DBParams db_params{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = m_kernel_cache_sizes.block_tree_db,
        };
        blockman.m_block_tree_db.reset();
        blockman.m_block_tree_db = std::make_unique<kernel::BlockTreeDB>(db_params);
        for (const auto& hash : side_hashes) {
            CDiskBlockIndex disk;
            BOOST_REQUIRE(blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, hash), disk));
            BOOST_CHECK(disk.ConstructBlockHash() == hash);
            BOOST_CHECK_EQUAL(disk.nHeight, 1);
            BOOST_CHECK(disk.hashPrev == chainstate.m_chain.Genesis()->GetBlockHash());
            BOOST_CHECK_EQUAL(disk.nStatus, BLOCK_VALID_TREE);
        }
        CDiskBlockIndex disk;
        BOOST_REQUIRE(blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, second_from_tip->GetBlockHash()), disk));
        BOOST_CHECK_EQUAL(disk.nStatus, second_from_tip->nStatus);
        BOOST_CHECK_EQUAL(disk.nDataPos, second_from_tip->nDataPos);
        BOOST_CHECK_EQUAL(disk.nUndoPos, second_from_tip->nUndoPos);
        kernel::CBlockFileInfo file_info;
        BOOST_REQUIRE(blockman.m_block_tree_db->ReadBlockFileInfo(disk.nFile, file_info));
        BOOST_CHECK_GT(file_info.nSize, disk.nDataPos);
        BOOST_CHECK_GT(file_info.nUndoSize, disk.nUndoPos);
        CBlock block;
        CBlockUndo undo;
        BOOST_CHECK(blockman.ReadBlock(block, *second_from_tip));
        BOOST_CHECK(blockman.ReadBlockUndo(undo, *second_from_tip));
    }
    node.validation_signals->UnregisterSharedValidationInterface(sub);
}

BOOST_AUTO_TEST_SUITE_END()
