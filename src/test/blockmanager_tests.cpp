// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <script/solver.h>
#include <primitives/block.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

using kernel::CBlockFileInfo;
using node::STORAGE_HEADER_BYTES;
using node::BlockManager;
using node::KernelNotifications;
using node::MAX_BLOCKFILE_SIZE;

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(legacy_blockmap_matches_original_type_and_growth)
{
    using OriginalMap = std::unordered_map<uint256, CBlockIndex, BlockHasher>;
    static_assert(std::is_same_v<node::BlockMap::Legacy, OriginalMap>);
    static_assert(!std::is_nothrow_invocable_v<BlockHasher, const uint256&>);
    node::BlockMap map{false};
    OriginalMap original;
    BOOST_CHECK(!map.IsCompact());
    for (uint64_t id = 1; id <= 10000; ++id) {
        const auto key{ArithToUint256(arith_uint256{id * 7919})};
        map.try_emplace(key);
        original.try_emplace(key);
        BOOST_CHECK_EQUAL(map.bucket_count(), original.bucket_count());
    }
    map.reserve(25000);
    original.reserve(25000);
    BOOST_CHECK_EQUAL(map.max_size(), original.max_size());
    BOOST_CHECK_EQUAL(map.max_load_factor(), original.max_load_factor());
    BOOST_CHECK_EQUAL(map.bucket_count(), original.bucket_count());
    auto expected{original.begin()};
    for (const auto& [key, index] : map) {
        BOOST_REQUIRE(expected != original.end());
        BOOST_CHECK(key == expected->first);
        ++expected;
    }
    BOOST_CHECK(expected == original.end());
    const auto first{map.begin()->first};
    map.erase(map.begin());
    original.erase(original.begin());
    BOOST_CHECK(!map.contains(first));
    BOOST_CHECK_EQUAL(map.size(), original.size());
}

BOOST_AUTO_TEST_CASE(compact_blockmap_keeps_addresses_and_keys_across_growth)
{
    for (bool compact : {false, true}) {
        node::BlockMap map{compact};
        BOOST_CHECK_EQUAL(map.IsCompact(), compact);
        std::vector<std::pair<const uint256*, CBlockIndex*>> original;
        for (uint64_t id = 1; id <= 20000; ++id) {
            // Identical low words also exercise full-key hashing/equality.
            const auto hash{ArithToUint256(arith_uint256{id} << 64)};
            auto [it, inserted]{map.try_emplace(hash)};
            BOOST_REQUIRE(inserted);
            it->second.phashBlock = &it->first;
            it->second.nHeight = id;
            if (!original.empty()) it->second.pprev = original.back().second;
            original.emplace_back(&it->first, &it->second);
            auto [duplicate, changed]{map.try_emplace(hash)};
            BOOST_CHECK(!changed);
            BOOST_CHECK(&duplicate->second == &it->second);
        }
        map.reserve(100000);
        BOOST_CHECK_EQUAL(map.size(), original.size());
        size_t visited{0};
        for (const auto& [key, index] : map) {
            BOOST_CHECK(index.GetBlockHash() == key);
            BOOST_CHECK(original[index.nHeight - 1].first == &key);
            BOOST_CHECK(original[index.nHeight - 1].second == &index);
            if (index.nHeight > 1) BOOST_CHECK(index.pprev == original[index.nHeight - 2].second);
            ++visited;
        }
        BOOST_CHECK_EQUAL(visited, map.size());
        const auto& constant{map};
        for (const auto& [key, index] : original) {
            BOOST_CHECK(&constant.find(*key)->second == index);
        }
        BOOST_CHECK(!constant.contains(ArithToUint256(arith_uint256{1} << 255)));
    }
}

BOOST_AUTO_TEST_CASE(compact_blockmap_erase_reinsert_and_work)
{
    node::BlockMap map{true};
    map.reserve(6000);
    for (uint64_t id = 1; id <= 6000; ++id) {
        auto [it, inserted]{map.try_emplace(ArithToUint256(arith_uint256{id}))};
        BOOST_REQUIRE(inserted);
        it->second.nHeight = id;
        it->second.phashBlock = &it->first;
        it->second.nChainWork = arith_uint256{1} << 200;
    }
    for (auto it = map.begin(); it != map.end();) {
        if (it->second.nHeight % 2) it = map.erase(it); else ++it;
    }
    BOOST_CHECK_EQUAL(map.size(), 3000);
    for (uint64_t id = 1; id <= 6000; ++id) {
        const auto key{ArithToUint256(arith_uint256{id})};
        BOOST_CHECK_EQUAL(map.contains(key), id % 2 == 0);
        auto [it, inserted]{map.try_emplace(key)};
        BOOST_CHECK_EQUAL(inserted, id % 2 != 0);
        it->second.nHeight = id;
        it->second.phashBlock = &it->first;
    }
    map.reserve(100000);
    BOOST_CHECK_EQUAL(map.size(), 6000);
    size_t count{0};
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        BOOST_CHECK(it->second.GetBlockHash() == it->first);
        if (it->second.nHeight % 2 == 0) BOOST_CHECK(it->second.nChainWork == (arith_uint256{1} << 200));
        ++count;
    }
    BOOST_CHECK_EQUAL(count, 6000);
    for (auto it = map.begin(); it != map.end();) it = map.erase(it);
    BOOST_CHECK(map.empty());
    BOOST_CHECK(map.begin() == map.end());
    BOOST_CHECK(map.try_emplace(uint256{}).second);
    BOOST_CHECK_THROW(map.reserve(map.max_size() + 1), std::length_error);
}

BOOST_AUTO_TEST_CASE(startup_height_sort_preserves_indexes)
{
    FastRandomContext random{true};
    for (size_t size : {0U, 1U, 64U, 4096U, 8192U, 65535U, 65536U, 65537U}) {
        std::vector<std::unique_ptr<CBlockIndex>> storage;
        std::vector<CBlockIndex*> indexes;
        for (size_t i = 0; i < size; ++i) {
            auto index{std::make_unique<CBlockIndex>()};
            index->nHeight = static_cast<int>(static_cast<int64_t>(random.randrange(uint64_t{1} << 32)) + std::numeric_limits<int>::min());
            if (i % 37 == 0) index->nHeight = std::numeric_limits<int>::min();
            if (i % 41 == 0) index->nHeight = std::numeric_limits<int>::max();
            indexes.push_back(index.get());
            storage.push_back(std::move(index));
        }
        auto expected{indexes};
        std::sort(expected.begin(), expected.end(), node::CBlockIndexHeightOnlyComparator());
        const std::set<CBlockIndex*> pointers{indexes.begin(), indexes.end()};
        node::SortBlockIndicesByHeight(indexes);
        BOOST_CHECK(std::is_sorted(indexes.begin(), indexes.end(), node::CBlockIndexHeightOnlyComparator()));
        const std::set<CBlockIndex*> result{indexes.begin(), indexes.end()};
        BOOST_CHECK(result == pointers);
        BOOST_REQUIRE_EQUAL(indexes.size(), expected.size());
        for (size_t i = 0; i < size; ++i) BOOST_CHECK_EQUAL(indexes[i]->nHeight, expected[i]->nHeight);
    }
}

BOOST_AUTO_TEST_CASE(startup_height_sort_preserves_repeated_pointers)
{
    std::array<CBlockIndex, 17> storage;
    for (size_t i = 0; i < storage.size(); ++i) storage[i].nHeight = static_cast<int>(i) - 8;
    std::vector<CBlockIndex*> indexes;
    for (size_t i = 0; i < 4096; ++i) indexes.push_back(&storage[(i * 11) % storage.size()]);
    const std::multiset<CBlockIndex*> expected{indexes.begin(), indexes.end()};
    node::SortBlockIndicesByHeight(indexes);
    BOOST_CHECK(std::is_sorted(indexes.begin(), indexes.end(), node::CBlockIndexHeightOnlyComparator()));
    const std::multiset<CBlockIndex*> actual{indexes.begin(), indexes.end()};
    BOOST_CHECK(actual == expected);
}

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};
    // simulate adding a genesis block normally
    BOOST_CHECK_EQUAL(blockman.WriteBlock(params->GenesisBlock(), 0).nPos, STORAGE_HEADER_BYTES);
    // simulate what happens during reindex
    // simulate a well-formed genesis block being found at offset 8 in the blk00000.dat file
    // the block is found at offset 8 because there is an 8 byte serialization header
    // consisting of 4 magic bytes + 4 length bytes before each block in a well-formed blk file.
    const FlatFilePos pos{0, STORAGE_HEADER_BYTES};
    blockman.UpdateBlockInfo(params->GenesisBlock(), 0, pos);
    // now simulate what happens after reindex for the first new block processed
    // the actual block contents don't matter, just that it's a block.
    // verify that the write position is at offset 0x12d.
    // this is a check to make sure that https://github.com/bitcoin/bitcoin/issues/21379 does not recur
    // 8 bytes (for serialization header) + 285 (for serialized genesis block) = 293
    // add another 8 bytes for the second block's serialization header and we get 293 + 8 = 301
    FlatFilePos actual{blockman.WriteBlock(params->GenesisBlock(), 1)};
    BOOST_CHECK_EQUAL(actual.nPos, STORAGE_HEADER_BYTES + ::GetSerializeSize(TX_WITH_WITNESS(params->GenesisBlock())) + STORAGE_HEADER_BYTES);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_loadblockindex_missing_file, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    blockman.WriteBlockIndexDB();
    std::vector<CBlockIndex*> ordered;
    BOOST_REQUIRE(blockman.LoadBlockIndexDB(std::nullopt, &ordered));
    BOOST_REQUIRE_EQUAL(ordered.size(), blockman.m_block_index.size());
    BOOST_CHECK(std::is_sorted(ordered.begin(), ordered.end(), node::CBlockIndexHeightOnlyComparator()));
    const std::set<CBlockIndex*> unique{ordered.begin(), ordered.end()};
    BOOST_CHECK_EQUAL(unique.size(), ordered.size());
    for (auto& [hash, index] : blockman.m_block_index) {
        BOOST_CHECK(unique.contains(&index));
    }
    const auto previous{ordered};

    // Only move a file inside the test fixture, never a user's datadir.
    const auto block_path{blockman.GetBlockPosFilename(chainman.ActiveTip()->GetBlockPos())};
    auto missing_path{block_path};
    missing_path += ".missing";
    fs::rename(block_path, missing_path);
    BOOST_CHECK(!blockman.LoadBlockIndexDB(std::nullopt, &ordered));
    BOOST_CHECK(ordered == previous); // Failed loads do not publish partial output.
    fs::rename(missing_path, block_path);
    BOOST_CHECK(blockman.LoadBlockIndexDB(std::nullopt, &ordered));
    BOOST_CHECK_EQUAL(ordered.size(), previous.size()); // Reload replaces, not appends.

}

// File-presence checks cover data on non-active/invalid branches, but must not
// retain references once those entries are pruned and the index is reloaded.
BOOST_FIXTURE_TEST_CASE(blockmanager_loadblockindex_preserves_unpersisted_header, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CBlock block{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), chainman.ActiveChainstate())};
    LOCK(::cs_main);
    blockman.WriteBlockIndexDB();
    block.nNonce = 0;
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
    CBlockIndex* ignored_best{nullptr};
    auto* unpersisted{blockman.AddToBlockIndex(block, ignored_best)};
    for (int reload = 0; reload < 2; ++reload) {
        std::vector<CBlockIndex*> ordered;
        BOOST_REQUIRE(blockman.LoadBlockIndexDB(std::nullopt, &ordered));
        const std::set<CBlockIndex*> unique{ordered.begin(), ordered.end()};
        BOOST_CHECK_EQUAL(ordered.size(), blockman.m_block_index.size());
        BOOST_CHECK_EQUAL(unique.size(), ordered.size());
        BOOST_CHECK_EQUAL(unique.count(unpersisted), 1U);
        for (auto& [hash, index] : blockman.m_block_index) BOOST_CHECK_EQUAL(unique.count(&index), 1U);
        BOOST_CHECK(std::is_sorted(ordered.begin(), ordered.end(), node::CBlockIndexHeightOnlyComparator()));
    }
}

BOOST_FIXTURE_TEST_CASE(blockmanager_loadblockindex_branch_file_and_pruning, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CBlock block{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), chainman.ActiveChainstate())};
    LOCK(::cs_main);
    block.hashPrevBlock = chainman.ActiveChain()[99]->GetBlockHash();
    block.nNonce = 0;
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
    CBlockIndex* ignored_best{nullptr};
    auto* index{blockman.AddToBlockIndex(block, ignored_best)};
    BOOST_REQUIRE(!chainman.ActiveChain().Contains(index));
    blockman.GetBlockFileInfo(0)->nSize = MAX_BLOCKFILE_SIZE;
    const FlatFilePos pos{blockman.WriteBlock(block, index->nHeight)};
    BOOST_REQUIRE_EQUAL(pos.nFile, 1);
    index->nFile = pos.nFile;
    index->nDataPos = pos.nPos;
    index->nTx = block.vtx.size();
    index->nStatus |= BLOCK_HAVE_DATA | BLOCK_FAILED_VALID;
    blockman.WriteBlockIndexDB();
    BOOST_REQUIRE(blockman.LoadBlockIndexDB(std::nullopt));

    const auto path{blockman.GetBlockPosFilename(pos)};
    auto missing{path};
    missing += ".missing";
    fs::rename(path, missing);
    BOOST_CHECK(!blockman.LoadBlockIndexDB(std::nullopt));
    fs::rename(missing, path);
    BOOST_CHECK(blockman.LoadBlockIndexDB(std::nullopt));

    blockman.PruneOneBlockFile(pos.nFile);
    blockman.WriteBlockIndexDB();
    fs::rename(path, missing);
    BOOST_CHECK(blockman.LoadBlockIndexDB(std::nullopt));
    BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
    fs::rename(missing, path);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_scan_unlink_already_pruned_files, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const CBlockIndex* old_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    WITH_LOCK(chainman.GetMutex(), blockman.GetBlockFileInfo(old_tip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    // Prune the older block file, but don't unlink it
    int file_number;
    {
        LOCK(chainman.GetMutex());
        file_number = old_tip->GetBlockPos().nFile;
        blockman.PruneOneBlockFile(file_number);
    }

    const FlatFilePos pos(file_number, 0);

    // Check that the file is not unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // if m_have_pruned is not yet set
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(!blockman.OpenBlockFile(pos, true).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(blockman.OpenBlockFile(pos, true).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman.GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!blockman.OpenBlockFile(new_pos, true).IsNull());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_availability, TestChain100Setup)
{
    // The goal of the function is to return the first not pruned block in the range [upper_block, lower_block].
    LOCK(::cs_main);
    auto& chainman = m_node.chainman;
    auto& blockman = chainman->m_blockman;
    const CBlockIndex& tip = *chainman->ActiveTip();

    // Function to prune all blocks from 'last_pruned_block' down to the genesis block
    const auto& func_prune_blocks = [&](CBlockIndex* last_pruned_block)
    {
        LOCK(::cs_main);
        CBlockIndex* it = last_pruned_block;
        while (it != nullptr && it->nStatus & BLOCK_HAVE_DATA) {
            it->nStatus &= ~BLOCK_HAVE_DATA;
            it = it->pprev;
        }
    };

    // 1) Return genesis block when all blocks are available
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), chainman->ActiveChain()[0]);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *chainman->ActiveChain()[0]));

    // 2) Check lower_block when all blocks are available
    CBlockIndex* lower_block = chainman->ActiveChain()[tip.nHeight / 2];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *lower_block));

    // Ensure we don't fail due to the expected absence of undo data in the genesis block
    CBlockIndex* upper_block = chainman->ActiveChain()[2];
    CBlockIndex* genesis = chainman->ActiveChain()[0];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(*upper_block, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    // Ensure we detect absence of undo data in the first block
    chainman->ActiveChain()[1]->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));

    // Prune half of the blocks
    int height_to_prune = tip.nHeight / 2;
    CBlockIndex* first_available_block = chainman->ActiveChain()[height_to_prune + 1];
    CBlockIndex* last_pruned_block = first_available_block->pprev;
    func_prune_blocks(last_pruned_block);

    // 3) The last block not pruned is in-between upper-block and the genesis block
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), first_available_block);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block));
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *last_pruned_block));

    // Simulate that the first available block is missing undo data and
    // detect this by using a status mask.
    first_available_block->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA}));
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(res);
        const auto& part{res.value()};
        BOOST_CHECK_EQUAL_COLLECTIONS(part.begin(), part.end(), block->begin() + offset, block->begin() + offset + size);
    }};

    expect_part(0, 20);
    expect_part(0, block->size() - 1);
    expect_part(0, block->size() - 10);
    expect_part(0, block->size());
    expect_part(1, block->size() - 1);
    expect_part(10, 20);
    expect_part(block->size() - 1, 1);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part_error, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part_error{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(!res);
        BOOST_CHECK_EQUAL(res.error(), node::ReadRawError::BadPartRange);
    }};

    expect_part_error(0, 0);
    expect_part_error(0, block->size() + 1);
    expect_part_error(0, std::numeric_limits<size_t>::max());
    expect_part_error(1, block->size());
    expect_part_error(2, block->size() - 1);
    expect_part_error(block->size() - 1, 2);
    expect_part_error(block->size() - 2, 3);
    expect_part_error(block->size() + 1, 0);
    expect_part_error(block->size() + 1, 1);
    expect_part_error(block->size() + 2, 2);
    expect_part_error(block->size(), 0);
    expect_part_error(block->size(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_readblock_hash_mismatch, TestingSetup)
{
    CBlockIndex index;
    {
        LOCK(cs_main);
        const auto tip{m_node.chainman->ActiveTip()};
        index.nStatus = tip->nStatus;
        index.nDataPos = tip->nDataPos;
        index.phashBlock = &uint256::ONE; // mismatched block hash
    }

    ASSERT_DEBUG_LOG("GetHash() doesn't match index");
    CBlock block;
    BOOST_CHECK(!m_node.chainman->m_blockman.ReadBlock(block, index));
}

BOOST_AUTO_TEST_CASE(blockmanager_flush_block_file)
{
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    node::BlockManager::Options blockman_opts{
        .chainparams = Params(),
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    // Test blocks with no transactions, not even a coinbase
    CBlock block1;
    block1.nVersion = 1;
    CBlock block2;
    block2.nVersion = 2;
    CBlock block3;
    block3.nVersion = 3;

    // They are 80 bytes header + 1 byte 0x00 for vtx length
    constexpr int TEST_BLOCK_SIZE{81};

    // Blockstore is empty
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), 0);

    // Write the first block to a new location.
    FlatFilePos pos1{blockman.WriteBlock(block1, /*nHeight=*/1)};

    // Write second block
    FlatFilePos pos2{blockman.WriteBlock(block2, /*nHeight=*/2)};

    // Two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // First two blocks are written as expected
    // Errors are expected because block data is junk, thrown AFTER successful read
    CBlock read_block;
    BOOST_CHECK_EQUAL(read_block.nVersion, 0);
    {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos1, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 1);
    }
    {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos2, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 2);
    }

    // During reindex, the flat file block storage will not be written to.
    // UpdateBlockInfo will, however, update the blockfile metadata.
    // Verify this behavior by attempting (and failing) to write block 3 data
    // to block 2 location.
    CBlockFileInfo* block_data = blockman.GetBlockFileInfo(0);
    BOOST_CHECK_EQUAL(block_data->nBlocks, 2);
    blockman.UpdateBlockInfo(block3, /*nHeight=*/3, /*pos=*/pos2);
    // Metadata is updated...
    BOOST_CHECK_EQUAL(block_data->nBlocks, 3);
    // ...but there are still only two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // Block 2 was not overwritten:
    BOOST_CHECK(!blockman.ReadBlock(read_block, pos2, {}));
    BOOST_CHECK_EQUAL(read_block.nVersion, 2);
}

BOOST_AUTO_TEST_SUITE_END()
