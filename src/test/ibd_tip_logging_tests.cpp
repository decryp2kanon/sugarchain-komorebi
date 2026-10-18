// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <kernel/notifications_interface.h>
#include <logging.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>

namespace {
// A pointer to the inherited base member lets tests call the protected method
// on a real Chainstate, without downcasting it or adding a production accessor.
struct TipLogAccess : Chainstate {
    static constexpr auto Update = &TipLogAccess::UpdateTip;
};

struct TipLogSetup : SteadyClockContext, TestChain100Setup {
    const BCLog::CategoryMask previous_categories{LogInstance().GetCategoryMask()};
    TipLogSetup() : TestChain100Setup{ChainType::REGTEST,
        TestOpts{.extra_args={"-nodebug", "-nodebuglogfile"}}}
    {
        // Other logger suites can leave categories enabled; argument parsing
        // alone does not reset this process-global logger in the unit fixture.
        LogInstance().DisableCategory(BCLog::ALL);
        BOOST_REQUIRE(!LogInstance().WillLogCategory(BCLog::VALIDATION));
    }
    ~TipLogSetup()
    {
        LogInstance().DisableCategory(BCLog::ALL);
        LogInstance().EnableCategory(BCLog::LogFlags{previous_categories});
    }

    CBlock HeaderAhead()
    {
        auto& manager{*m_node.chainman};
        auto block{CreateBlock({}, CScript{} << OP_TRUE, manager.ActiveChainstate())};
        BlockValidationState state;
        const std::array<CBlockHeader, 1> headers{block};
        BOOST_REQUIRE(manager.ProcessNewBlockHeaders(headers, true, state, nullptr));
        manager.m_cached_is_ibd = true;
        *this += 2s;
        return block;
    }
};

struct RestoreCategories {
    BCLog::CategoryMask previous{LogInstance().GetCategoryMask()};
    ~RestoreCategories()
    {
        LogInstance().DisableCategory(BCLog::ALL);
        LogInstance().EnableCategory(BCLog::LogFlags{previous});
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(ibd_tip_logging_tests, TipLogSetup)

BOOST_AUTO_TEST_CASE(progress_interval_preserves_state_and_notifications)
{
    HeaderAhead();
    auto& chainstate{m_node.chainman->ActiveChainstate()};
    std::atomic<unsigned> lines{0};
    DebugLogHelper log{"UpdateTip: new best=", [&](const std::string* line) {
        if (line) ++lines;
        return false;
    }};
    LOCK(cs_main);
    const auto* tip{chainstate.m_chain.Tip()};
    const auto best{chainstate.CoinsTip().GetBestBlock()};
    const auto updates{m_node.mempool->GetTransactionsUpdated()};
    auto update = [&] { (chainstate.*TipLogAccess::Update)(tip); };
    update();
    BOOST_CHECK_EQUAL(lines.load(), 1U);
    update();
    *this += 999ms;
    update();
    BOOST_CHECK_EQUAL(lines.load(), 1U);
    *this += 1ms;
    update();
    BOOST_CHECK_EQUAL(lines.load(), 2U);
    *this += 5s;
    update();
    for (int i{0}; i < 100; ++i) update();
    BOOST_CHECK_EQUAL(lines.load(), 3U);
    BOOST_CHECK_EQUAL(m_node.mempool->GetTransactionsUpdated(), updates + 105);
    BOOST_CHECK(chainstate.m_chain.Tip() == tip);
    BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == best);
    // Clock mocking/reset must not suppress progress until the old time returns.
    MockableSteadyClock::SetMockTime(1ms);
    update();
    BOOST_CHECK_EQUAL(lines.load(), 4U);
}

BOOST_AUTO_TEST_CASE(normal_operation_and_debug_keep_every_tip)
{
    HeaderAhead();
    auto& manager{*m_node.chainman};
    auto& chainstate{manager.ActiveChainstate()};
    RestoreCategories restore;
    std::atomic<unsigned> lines{0};
    DebugLogHelper log{"UpdateTip: new best=", [&](const std::string* line) {
        if (line) ++lines;
        return false;
    }};
    LOCK(cs_main);
    manager.m_cached_is_ibd = false;
    for (int i{0}; i < 3; ++i) (chainstate.*TipLogAccess::Update)(chainstate.m_chain.Tip());
    BOOST_CHECK_EQUAL(lines.load(), 3U);
    manager.m_cached_is_ibd = true;
    LogInstance().EnableCategory(BCLog::VALIDATION);
    for (int i{0}; i < 3; ++i) (chainstate.*TipLogAccess::Update)(chainstate.m_chain.Tip());
    BOOST_CHECK_EQUAL(lines.load(), 6U);
}

BOOST_AUTO_TEST_CASE(catch_up_keeps_the_final_tip)
{
    const auto block{HeaderAhead()};
    auto& manager{*m_node.chainman};
    std::atomic<unsigned> lines{0};
    DebugLogHelper log{"UpdateTip: new best=", [&](const std::string* line) {
        if (line) ++lines;
        return false;
    }};
    bool added{false};
    BOOST_REQUIRE(manager.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &added));
    BOOST_REQUIRE(added);
    BOOST_CHECK_EQUAL(lines.load(), 1U);
    LOCK(cs_main);
    manager.m_cached_is_ibd = true;
    auto& chainstate{manager.ActiveChainstate()};
    BOOST_REQUIRE(chainstate.m_chain.Tip() == manager.m_best_header);
    (chainstate.*TipLogAccess::Update)(chainstate.m_chain.Tip());
    BOOST_CHECK_EQUAL(lines.load(), 2U);
}

BOOST_AUTO_TEST_CASE(presync_logs_every_24000_headers_without_throttling_notifications)
{
    struct Notifications : kernel::Notifications {
        unsigned calls{0};
        void headerTip(SynchronizationState, int64_t, int64_t, bool presync) override
        {
            BOOST_CHECK(presync);
            ++calls;
        }
    } notifications;
    // Regtest's built-in minimum work is zero, so use real mainnet parameters
    // with a local empty header tip to exercise the presync reporting path.
    const auto params{CChainParams::Main()};
    CBlockIndex best;
    fs::create_directories(m_path_root / "presync" / "blocks");
    ChainstateManager manager{m_interrupt,
        {.chainparams = *params, .datadir = m_path_root / "presync",
         .notifications = notifications, .script_execution_cache_bytes = 0,
         .signature_cache_bytes = 0},
        {.chainparams = *params, .blocks_dir = m_path_root / "presync" / "blocks",
         .notifications = notifications,
         .block_tree_db_params = {.path = m_path_root / "presync" / "index", .cache_bytes = 0, .memory_only = true}}};
    {
        LOCK(cs_main);
        manager.m_best_header = &best;
    }
    std::atomic<unsigned> lines{0};
    DebugLogHelper log{"Pre-synchronizing blockheaders, height:", [&](const std::string* line) {
        if (line) ++lines;
        return false;
    }};
    auto report = [&](int64_t height) {
        *this += 250ms;
        manager.ReportHeadersPresync(height, 1);
    };
    report(2000); // Preserve the first available observation.
    for (int64_t height{4000}; height <= 24000; height += 2000) report(height);
    report(25999);
    BOOST_CHECK_EQUAL(lines.load(), 1U);
    BOOST_CHECK_EQUAL(notifications.calls, 13U);
    manager.ReportHeadersPresync(26000, 1); // Original 250ms gate still applies.
    BOOST_CHECK_EQUAL(notifications.calls, 13U);
    report(26000);
    BOOST_CHECK_EQUAL(lines.load(), 2U);
    report(26000); // Repeated heights must not consume the log budget.
    BOOST_CHECK_EQUAL(lines.load(), 2U);
    report(1000); // A lower restart/peer height resets the logging interval.
    BOOST_CHECK_EQUAL(lines.load(), 3U);
    report(25000);
    BOOST_CHECK_EQUAL(lines.load(), 4U);
    BOOST_CHECK_EQUAL(notifications.calls, 17U);
    {
        LOCK(cs_main);
        BOOST_CHECK(manager.m_best_header == &best);
        BOOST_CHECK_EQUAL(best.nHeight, 0);
    }
    manager.m_cached_is_ibd = false;
    report(49000);
    BOOST_CHECK_EQUAL(lines.load(), 4U);
    BOOST_CHECK_EQUAL(notifications.calls, 18U);
    {
        LOCK(cs_main);
        best.nChainWork = UintToArith256(params->GetConsensus().nMinimumChainWork);
    }
    report(73000); // Keep the existing post-minchainwork early return.
    BOOST_CHECK_EQUAL(lines.load(), 4U);
    BOOST_CHECK_EQUAL(notifications.calls, 18U);
}

BOOST_AUTO_TEST_SUITE_END()
