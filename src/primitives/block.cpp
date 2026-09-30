// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <crypto/yespower-1.0.1/yespower.h>
#include <streams.h>
#include <stdexcept>
#include <tinyformat.h>

#include <memory>
#include <span>
#include <sstream>

namespace {
// Each verification worker owns its scratch allocation. Unlike yespower_tls,
// release it when the thread exits, including when a worker pool is destroyed.
struct YespowerLocal {
    yespower_local_t memory;
    YespowerLocal()
    {
        if (yespower_init_local(&memory)) throw std::runtime_error("YespowerSugar: failed to initialize scratch memory");
    }
    ~YespowerLocal() { yespower_free_local(&memory); }
    YespowerLocal(const YespowerLocal&) = delete;
    YespowerLocal& operator=(const YespowerLocal&) = delete;
};
} // namespace

uint256 CBlockHeader::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

// Official Sugarchain YespowerSugar parameters. The block identifier remains SHA256d.
uint256 CBlockHeader::GetPoWHash() const
{
    static_assert(YESPOWER_SUGAR_VERSION == YESPOWER_1_0);
    static constexpr yespower_params_t params{YESPOWER_1_0, YESPOWER_SUGAR_N, YESPOWER_SUGAR_R,
                                               YESPOWER_SUGAR_PERSONALIZATION,
                                               sizeof(YESPOWER_SUGAR_PERSONALIZATION) - 1};
    DataStream serialized;
    serialized << *this;
    yespower_binary_t result;
    static thread_local YespowerLocal local;
    if (yespower(&local.memory, reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size(), &params, &result)) {
        throw std::runtime_error("YespowerSugar: failed to compute proof of work");
    }
    uint256 hash;
    std::copy(std::begin(result.uc), std::end(result.uc), hash.begin());
    return hash;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
