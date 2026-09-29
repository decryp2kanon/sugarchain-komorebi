// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <common/args.h>
#include <node/miner.h>
#include <node/ibd_download.h>
#include <node/peerman_args.h>
#include <net_processing.h>
#include <pow.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <limits>

BOOST_FIXTURE_TEST_SUITE(peerman_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(header_pow_default_and_explicit_override)
{
    ArgsManager args;
    PeerManager::Options options;
    node::ApplyArgsManOptions(args, options);
    BOOST_CHECK_EQUAL(options.header_pow_workers, 8);
    BOOST_CHECK_EQUAL(MAX_HEADER_POW_WORKERS, 8);
    // Configuration bounds only: this test does not launch worker threads.
    for (const auto& [value, expected] : std::vector<std::pair<const char*, int>>{
             {"1", 1}, {"2", 2}, {"4", 4}, {"8", 8}, {"0", 1}, {"16", 8}}) {
        args.ForceSetArg("-parpow", value);
        node::ApplyArgsManOptions(args, options);
        BOOST_CHECK_EQUAL(options.header_pow_workers, expected);
    }
}

BOOST_AUTO_TEST_CASE(ibd_delivery_budget_is_bounded_and_ignores_idle_time)
{
    node::IBDBlockDelivery delivery;
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 128);
    BOOST_CHECK_EQUAL(delivery.Limit(16, 16), 16);
    delivery.Received(1'000'000, true);
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 128);
    delivery.Received(1'000'000, true); // Same timestamp is not an infinite rate.
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 128);
    delivery.Received(1'020'000, true); // Slow 20ms service stays at upstream floor.
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 16);
    int64_t now{1'020'000};
    for (int i{0}; i < 100; ++i) {
        delivery.Received(now += 1'000, true);
        BOOST_CHECK_GE(delivery.Limit(16, 128), 16);
        BOOST_CHECK_LE(delivery.Limit(16, 128), 128);
        BOOST_CHECK_EQUAL(delivery.Limit(16, 16), 16);
        BOOST_CHECK_LE(delivery.Limit(16, 64), 64);
    }
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 128);
    delivery.Received(now += 1'000, false); // Drain the queue.
    delivery.Received(now += 60'000'000, true); // New request after idle gap.
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 128);
    for (int i{0}; i < 100; ++i) delivery.Received(now += 20'000, true);
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 16);
    delivery.Received(-1, true);
    delivery.Received(0, true);
    delivery.Received(1, true);
    delivery.Received(std::numeric_limits<int64_t>::max(), true);
    BOOST_CHECK_EQUAL(delivery.Limit(16, 128), 16);
    node::IBDBlockDelivery new_peer;
    BOOST_CHECK_EQUAL(new_peer.Limit(16, 128), 128);
    // A high RTT must not be mistaken for low service throughput: retain
    // enough outstanding requests to cover the first-response latency too.
    node::IBDBlockDelivery distant;
    distant.Started(1'000'000);
    distant.Received(2'000'000, true);
    distant.Received(2'004'000, true);
    BOOST_CHECK_EQUAL(distant.Limit(16, 128), 128);
    BOOST_CHECK_EQUAL(distant.Limit(16, 64), 64);
    distant.Received(2'008'000, false);
    distant.Started(5'000'000);
    distant.Received(5'050'000, true);
    distant.Received(5'054'000, true);
    BOOST_CHECK_EQUAL(distant.Limit(16, 128), 75);
    distant.Started(65'000'000); // Previous requests were cancelled while pending.
    distant.Received(65'050'000, true);
    distant.Received(65'054'000, true);
    BOOST_CHECK_EQUAL(distant.Limit(16, 128), 75);
    distant.Started(-1);
    distant.Received(1, true);
    distant.Started(10);
    distant.Received(std::numeric_limits<int64_t>::max(), true);
    BOOST_CHECK_GE(distant.Limit(16, 128), 16);
    BOOST_CHECK_LE(distant.Limit(16, 128), 128);
}

BOOST_AUTO_TEST_CASE(ibd_request_budget_is_opt_in_and_bounded)
{
    ArgsManager args;
    PeerManager::Options options;
    node::ApplyArgsManOptions(args, options);
    BOOST_CHECK_EQUAL(options.ibd_block_request_limit, 16);
    for (const auto& [text, expected] : std::vector<std::pair<const char*, int>>{
             {"-9223372036854775808", 16}, {"0", 16}, {"15", 16}, {"16", 16},
             {"64", 64}, {"128", 128}, {"129", 128}, {"9223372036854775807", 128}}) {
        args.ForceSetArg("-maxibdblocksinflight", text);
        node::ApplyArgsManOptions(args, options);
        BOOST_CHECK_EQUAL(options.ibd_block_request_limit, expected);
        BOOST_CHECK_EQUAL(options.header_pow_workers, DEFAULT_HEADER_POW_WORKERS);
    }
}

/** Window, in blocks, for connecting to NODE_NETWORK_LIMITED peers */
static constexpr int64_t NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS = 144;

static void mineBlock(const node::NodeContext& node, std::chrono::seconds block_time)
{
    auto curr_time = GetTime<std::chrono::seconds>();
    node::BlockAssembler::Options options;
    options.include_dummy_extranonce = true;
    SetMockTime(block_time); // update time so the block is created with it
    CBlock block = node::BlockAssembler{node.chainman->ActiveChainstate(), nullptr, options}.CreateNewBlock()->block;
    while (!CheckProofOfWork(block.GetHash(), block.nBits, node.chainman->GetConsensus())) ++block.nNonce;
    block.fChecked = true; // little speedup
    SetMockTime(curr_time); // process block at current time
    Assert(node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, nullptr));
    node.validation_signals->SyncWithValidationInterfaceQueue(); // drain events queue
}

// Verifying when network-limited peer connections are desirable based on the node's proximity to the tip
BOOST_AUTO_TEST_CASE(connections_desirable_service_flags)
{
    std::unique_ptr<PeerManager> peerman = PeerManager::make(*m_node.connman, *m_node.addrman, nullptr, *m_node.chainman, *m_node.mempool, *m_node.warnings, {});
    auto consensus = m_node.chainman->GetParams().GetConsensus();

    // Check we start connecting to full nodes
    ServiceFlags peer_flags{NODE_WITNESS | NODE_NETWORK_LIMITED};
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // Make peerman aware of the initial best block and verify we accept limited peers when we start close to the tip time.
    auto tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
    uint64_t tip_block_time = tip->GetBlockTime();
    int tip_block_height = tip->nHeight;
    peerman->SetBestBlock(tip_block_height, std::chrono::seconds{tip_block_time});

    SetMockTime(tip_block_time + 1); // Set node time to tip time
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Check we don't disallow limited peers connections when we are behind but still recoverable (below the connection safety window)
    SetMockTime(GetTime<std::chrono::seconds>() + std::chrono::seconds{consensus.nPowTargetSpacing * (NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS - 1)});
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Check we disallow limited peers connections when we are further than the limited peers safety window
    SetMockTime(GetTime<std::chrono::seconds>() + std::chrono::seconds{consensus.nPowTargetSpacing * 2});
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // By now, we tested that the connections desirable services flags change based on the node's time proximity to the tip.
    // Now, perform the same tests for when the node receives a block.
    m_node.validation_signals->RegisterValidationInterface(peerman.get());

    // First, verify a block in the past doesn't enable limited peers connections
    // At this point, our time is (NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS + 1) * 10 minutes ahead the tip's time.
    mineBlock(m_node, /*block_time=*/std::chrono::seconds{tip_block_time + 1});
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // Verify a block close to the tip enables limited peers connections
    mineBlock(m_node, /*block_time=*/GetTime<std::chrono::seconds>());
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Lastly, verify the stale tip checks can disallow limited peers connections after not receiving blocks for a prolonged period.
    SetMockTime(GetTime<std::chrono::seconds>() + std::chrono::seconds{consensus.nPowTargetSpacing * NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS + 1});
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));
}

BOOST_AUTO_TEST_SUITE_END()
