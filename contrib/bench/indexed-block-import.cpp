// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Isolated validation component benchmark. No network peers or public IBD.
// The caller's min_pow_checked precondition is supplied by this harness to
// isolate an indexed-header/block stage with short, genuine historical fixtures.
// This is NOT a test of reaching mainnet's minimum-chainwork threshold.
#include <consensus/validation.h>
#include <chainparams.h>
#include <crypto/common.h>
#include <kernel/coinstats.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <validation.h>

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{};
unsigned long long YespowerProfileCalls();
unsigned long long YespowerProfileCPU();

int main(int argc, char** argv)
{
    try {
        if (argc < 3 || argc > 5 || (std::string_view{argv[2]} != "warm" && std::string_view{argv[2]} != "cold")) {
            throw std::runtime_error("Usage: indexed-block-import BLOCK_FILE warm|cold [SCRIPT_WORKERS(0-15) [DBCACHE_MIB(4-16384)]]");
        }
        auto number = [](std::string_view input, int minimum, int maximum) {
            int value{};
            const auto [end, error]{std::from_chars(input.data(), input.data() + input.size(), value)};
            if (error != std::errc{} || end != input.data() + input.size() || value < minimum || value > maximum) {
                throw std::runtime_error("Invalid benchmark resource limit");
            }
            return value;
        };
        const int script_workers{argc >= 4 ? number(argv[3], 0, MAX_SCRIPTCHECK_THREADS) : 2};
        const int dbcache{argc >= 5 ? number(argv[4], 4, 16384) : 512};
        const std::string dbcache_arg{"-dbcache=" + std::to_string(dbcache)};
        const auto setup{MakeNoLogFileContext<ChainTestingSetup>(ChainType::MAIN,
            TestOpts{.extra_args={"-checkblockindex=0", "-assumevalid=0", dbcache_arg.c_str()},
                     .coins_db_in_memory=false, .block_tree_db_in_memory=false, .setup_net=false})};
        // Unit fixtures hard-code a full index audit after every header/block.
        // Match production mainnet's diagnostic policy on BOTH benchmark arms;
        // keep the regression tests' exhaustive audits enabled separately.
        auto& node{setup->m_node};
        node.chainman.reset(); // The setup has not loaded any chainstate yet.
        ChainstateManager::Options options{
            .chainparams=Params(), .datadir=setup->m_args.GetDataDirNet(),
            .check_block_index=0, .notifications=*node.notifications,
            .signals=node.validation_signals.get(), .worker_threads_num=script_workers,
        };
        node::BlockManager::Options storage{
            .chainparams=options.chainparams, .blocks_dir=setup->m_args.GetBlocksDirPath(),
            .notifications=options.notifications,
            .block_tree_db_params=DBParams{
                .path=setup->m_args.GetDataDirNet()/"blocks"/"index",
                .cache_bytes=setup->m_kernel_cache_sizes.block_tree_db, .memory_only=false,
            },
        };
        node.chainman = std::make_unique<ChainstateManager>(*node.shutdown_signal, options, storage);
        setup->m_coins_db_in_memory = false;
        setup->LoadVerifyActivateChainstate();
        auto& chainman{*setup->m_node.chainman};
        std::ifstream input{argv[1], std::ios::binary};
        if (!input) throw std::runtime_error("Cannot open fixture");
        std::vector<std::shared_ptr<CBlock>> blocks;
        std::vector<CBlockHeader> headers;
        uint64_t transactions{0}, non_coinbase_inputs{0}, fixture_bytes{0};
        std::array<unsigned char, 8> prefix;
        while (input.read(reinterpret_cast<char*>(prefix.data()), prefix.size())) {
            if (!std::equal(prefix.begin(), prefix.begin()+4, chainman.GetParams().MessageStart().begin())) throw std::runtime_error("Wrong magic");
            const auto size{ReadLE32(prefix.data()+4)};
            if (size <= 80 || size > 4'000'000 || blocks.size() >= 100'000) throw std::runtime_error("Fixture bounds");
            std::vector<std::byte> data(size);
            if (!input.read(reinterpret_cast<char*>(data.data()), size)) throw std::runtime_error("Truncated block");
            DataStream stream{data};
            auto block{std::make_shared<CBlock>()};
            stream >> TX_WITH_WITNESS(*block);
            if (!stream.empty()) throw std::runtime_error("Trailing block data");
            transactions += block->vtx.size();
            fixture_bytes += size + prefix.size();
            for (const auto& tx : block->vtx) {
                if (!tx->IsCoinBase()) non_coinbase_inputs += tx->vin.size();
            }
            headers.push_back(*block);
            blocks.push_back(std::move(block));
        }
        if (!input.eof() || input.gcount() != 0 || blocks.empty()) throw std::runtime_error("Truncated/empty fixture or read failure");
        std::cerr << "INDEXED_FIXTURE {\"script_workers\":" << script_workers
                  << ",\"dbcache_mib\":" << dbcache << ",\"transactions\":" << transactions
                  << ",\"non_coinbase_inputs\":" << non_coinbase_inputs
                  << ",\"fixture_bytes\":" << fixture_bytes << "}" << std::endl;
        InitYespowerVerificationCache(DEFAULT_YESPOWER_CACHE_BYTES);
        auto run = [&](const char* name, auto action) {
            const auto calls{YespowerProfileCalls()}, cpu{YespowerProfileCPU()};
            const auto start{std::chrono::steady_clock::now()};
            action();
            std::cout << name << ',' << blocks.size() << ','
                      << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << ','
                      << YespowerProfileCalls()-calls << ',' << YespowerProfileCPU()-cpu << std::endl;
        };
        std::cout << "phase,blocks,seconds,yespower_calls,yespower_cpu_ns\n";
        run("headers", [&] {
            HeaderPoWVerifier verifier{8};
            if (!verifier.Check(headers, chainman.GetConsensus())) throw std::runtime_error("Invalid proof");
            BlockValidationState state;
            if (!chainman.ProcessNewBlockHeaders(headers, true, state, nullptr)) throw std::runtime_error(state.ToString());
        });
        if (std::string_view{argv[2]} == "cold") InitYespowerVerificationCache(DEFAULT_YESPOWER_CACHE_BYTES);
        run(argv[2], [&] {
            for (const auto& block : blocks) {
                bool added{false};
                if (!chainman.ProcessNewBlock(block, true, true, &added) || !added) throw std::runtime_error("Block rejected");
            }
        });
        {
            LOCK(cs_main);
            auto& chainstate{chainman.ActiveChainstate()};
            if (chainstate.m_chain.Height() != int(blocks.size()) || chainstate.m_chain.Tip()->GetBlockHash() != blocks.back()->GetHash()) throw std::runtime_error("Wrong final tip");
            chainstate.ForceFlushStateToDisk();
            const auto stats{kernel::ComputeUTXOStats(kernel::CoinStatsHashType::HASH_SERIALIZED, &chainstate.CoinsDB(), chainman.m_blockman)};
            if (!stats) throw std::runtime_error("UTXO hash failed");
            std::cout << "tip=" << blocks.back()->GetHash().ToString() << " utxo=" << stats->hashSerialized.ToString() << std::endl;
            if (CVerifyDB{chainman.GetNotifications()}.VerifyDB(chainstate, chainman.GetConsensus(), chainstate.CoinsTip(), 4, blocks.size()) != VerifyDBResult::SUCCESS) throw std::runtime_error("verifychain failed");
            std::cout << "verifychain=PASS\n";
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
