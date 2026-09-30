// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <consensus/validation.h>
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

BOOST_AUTO_TEST_SUITE_END()
