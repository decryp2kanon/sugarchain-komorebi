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

uint256 CBlockHeader::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

// Official Sugarchain YespowerSugar parameters. The block identifier remains SHA256d.
uint256 CBlockHeader::GetPoWHash() const
{
    static constexpr uint8_t personalization[] =
        "Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote";
    static constexpr yespower_params_t params{YESPOWER_1_0, 2048, 32,
                                               personalization, sizeof(personalization) - 1};
    DataStream serialized;
    serialized << *this;
    yespower_binary_t result;
    if (yespower_tls(reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size(), &params, &result)) {
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
