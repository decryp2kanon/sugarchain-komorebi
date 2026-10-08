// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chain.h>
#include <test/util/setup_common.h>

#include <memory>

BOOST_FIXTURE_TEST_SUITE(chain_tests, BasicTestingSetup)

namespace {

const CBlockIndex* NaiveGetAncestor(const CBlockIndex* a, int height)
{
    while (a->nHeight > height) {
        a = a->pprev;
    }
    BOOST_REQUIRE_EQUAL(a->nHeight, height);
    return a;
}

const CBlockIndex* NaiveLastCommonAncestor(const CBlockIndex* a, const CBlockIndex* b)
{
    while (a->nHeight > b->nHeight) {
        a = a->pprev;
    }
    while (b->nHeight > a->nHeight) {
        b = b->pprev;
    }
    while (a != b) {
        BOOST_REQUIRE_EQUAL(a->nHeight, b->nHeight);
        a = a->pprev;
        b = b->pprev;
    }
    BOOST_REQUIRE_EQUAL(a, b);
    return a;
}

} // namespace

// Compare with the original 256-bit formula across every compact exponent,
// signed/overflow/zero encodings, and boundaries around powers of two.
BOOST_AUTO_TEST_CASE(bits_proof_exact_arithmetic)
{
    const auto reference = [](uint32_t bits) -> arith_uint256 {
        arith_uint256 target;
        bool negative{false}, overflow{false};
        target.SetCompact(bits, &negative, &overflow);
        if (negative || overflow || target == 0) return arith_uint256{0};
        return (~target / (target + 1)) + 1;
    };
    FastRandomContext random{true};
    for (uint32_t size = 0; size < 256; ++size) {
        for (uint32_t word : {0U, 1U, 2U, 3U, 0xffU, 0x100U, 0xffffU, 0x10000U, 0x7fffffU, 0x800000U, 0xffffffU}) {
            const uint32_t bits{(size << 24) | word};
            BOOST_CHECK(GetBitsProof(bits) == reference(bits));
        }
        for (int i = 0; i < 1000; ++i) {
            const uint32_t bits{(size << 24) | (random.rand32() & 0xffffff)};
            BOOST_CHECK(GetBitsProof(bits) == reference(bits));
        }
    }
}

BOOST_AUTO_TEST_CASE(set_tip_preserves_chain_across_extensions_and_reorgs)
{
    std::vector<CBlockIndex> main(128);
    for (size_t i = 0; i < main.size(); ++i) {
        main[i].nHeight = i;
        main[i].pprev = i ? &main[i - 1] : nullptr;
        main[i].BuildSkip();
    }
    CChain chain;
    BOOST_CHECK_EQUAL(chain.Height(), -1);
    chain.SetTip(main[127]);
    BOOST_REQUIRE_EQUAL(chain.Height(), 127);
    for (int height = 0; height <= 127; ++height) BOOST_CHECK(chain[height] == &main[height]);
    chain.SetTip(main[63]);
    BOOST_CHECK_EQUAL(chain.Height(), 63);
    BOOST_CHECK(chain[64] == nullptr);
    chain.SetTip(main[127]);

    // A branch without skip links must retain identical parent-link semantics.
    std::vector<CBlockIndex> fork(80);
    for (size_t i = 0; i < fork.size(); ++i) {
        fork[i].nHeight = 64 + i;
        fork[i].pprev = i ? &fork[i - 1] : &main[63];
    }
    chain.SetTip(fork.back());
    BOOST_REQUIRE_EQUAL(chain.Height(), 143);
    for (int height = 0; height < 64; ++height) BOOST_CHECK(chain[height] == &main[height]);
    for (size_t i = 0; i < fork.size(); ++i) BOOST_CHECK(chain[64 + i] == &fork[i]);
    BOOST_CHECK(!chain.Contains(&main[127]));
    chain.SetTip(main[127]);
    for (int height = 0; height <= 127; ++height) BOOST_CHECK(chain[height] == &main[height]);
    chain.SetTip(main[0]);
    BOOST_CHECK(chain.Genesis() == &main[0]);
    BOOST_CHECK(chain.Tip() == &main[0]);
    BOOST_CHECK(chain[1] == nullptr);
}

BOOST_AUTO_TEST_CASE(chain_test)
{
    FastRandomContext ctx;
    std::vector<std::unique_ptr<CBlockIndex>> block_index;
    // Run 10 iterations of the whole test.
    for (int i = 0; i < 10; ++i) {
        block_index.clear();
        // Create genesis block.
        auto genesis = std::make_unique<CBlockIndex>();
        genesis->nHeight = 0;
        block_index.push_back(std::move(genesis));
        // Create 10000 more blocks.
        for (int b = 0; b < 10000; ++b) {
            auto new_index = std::make_unique<CBlockIndex>();
            // 95% of blocks build on top of the last block; the others fork off randomly.
            if (ctx.randrange(20) != 0) {
                new_index->pprev = block_index.back().get();
            } else {
                new_index->pprev = block_index[ctx.randrange(block_index.size())].get();
            }
            new_index->nHeight = new_index->pprev->nHeight + 1;
            new_index->BuildSkip();
            block_index.push_back(std::move(new_index));
        }
        // Run 10000 random GetAncestor queries.
        for (int q = 0; q < 10000; ++q) {
            const CBlockIndex* block = block_index[ctx.randrange(block_index.size())].get();
            unsigned height = ctx.randrange<unsigned>(block->nHeight + 1);
            const CBlockIndex* result = block->GetAncestor(height);
            BOOST_CHECK(result == NaiveGetAncestor(block, height));
        }
        // Run 10000 random LastCommonAncestor queries.
        for (int q = 0; q < 10000; ++q) {
            const CBlockIndex* block1 = block_index[ctx.randrange(block_index.size())].get();
            const CBlockIndex* block2 = block_index[ctx.randrange(block_index.size())].get();
            const CBlockIndex* result = LastCommonAncestor(block1, block2);
            BOOST_CHECK(result == NaiveLastCommonAncestor(block1, block2));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
