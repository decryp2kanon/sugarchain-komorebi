// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2016-2018 The Zcash developers
// Copyright (c) 2018-2020 The Sugarchain Yumekawa developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <algorithm>
#include <chain.h>
#include <checkqueue.h>
#include <crypto/sha256.h>
#include <cuckoocache.h>
#include <primitives/block.h>
#include <random.h>
#include <uint256.h>
#include <util/check.h>
#include <util/hasher.h>

#include <mutex>
#include <exception>
#include <limits>
#include <shared_mutex>
#include <stdexcept>

namespace {
static_assert(MAX_YESPOWER_CACHE_BYTES / sizeof(uint256) <= std::numeric_limits<uint32_t>::max() / 45);
// As with the signature cache, store only successful verification, under a
// salted digest of the entire input. This is bounded, process-local evidence:
// neither a peer nor a persisted block-index status can populate it.
class YespowerVerificationCache {
    using Cache = CuckooCache::cache<uint256, SignatureCacheHasher>;
    CSHA256 m_hasher;
    std::unique_ptr<Cache> m_valid{std::make_unique<Cache>()};
    std::unique_ptr<Cache> m_batches;
    std::shared_mutex m_mutex;

public:
    YespowerVerificationCache()
    {
        const auto nonce{GetRandHash()};
        m_hasher.Write(nonce.begin(), nonce.size());
        Reset(DEFAULT_YESPOWER_CACHE_BYTES);
    }

    void Reset(size_t bytes)
    {
        auto replacement{std::make_unique<Cache>()};
        // Reserve within the configured budget, not in addition to it. Tiny
        // test caches keep their original minimum-allocation behavior.
        const size_t batch_bytes{bytes >= 1024 ? std::min(bytes / 8, size_t{2} << 20) : 0};
        std::unique_ptr<Cache> batches;
        if (batch_bytes) {
            batches = std::make_unique<Cache>();
            batches->setup_bytes(batch_bytes);
        }
        replacement->setup_bytes(bytes - batch_bytes);
        std::unique_lock lock{m_mutex};
        std::swap(m_valid, replacement);
        std::swap(m_batches, batches);
    }

    uint256 Entry(const CBlockHeader& header) const
    {
        // GetHash commits to all 80 serialized bytes, including nBits. The
        // YespowerSugar algorithm and personalization are fixed in GetPoWHash.
        const auto hash{header.GetHash()};
        uint256 entry;
        CSHA256{m_hasher}.Write(hash.begin(), hash.size()).Finalize(entry.begin());
        return entry;
    }

    bool Contains(const uint256& entry)
    {
        std::shared_lock lock{m_mutex};
        return m_valid->contains(entry, false);
    }

    void Insert(const uint256& entry)
    {
        std::unique_lock lock{m_mutex};
        m_valid->insert(entry);
    }

    uint256 BatchEntry(std::span<const CBlockHeader> headers) const
    {
        // Bind order, count and all header bytes. Separate the batch domain
        // from individual proofs; neither kind of evidence survives restart.
        const unsigned char domain{1};
        unsigned char count[8];
        WriteLE64(count, headers.size());
        auto hasher{m_hasher};
        hasher.Write(&domain, 1).Write(count, sizeof(count));
        for (const auto& header : headers) {
            const auto hash{header.GetHash()};
            hasher.Write(hash.begin(), hash.size());
        }
        uint256 entry;
        hasher.Finalize(entry.begin());
        return entry;
    }

    bool RestoreBatch(const uint256& entry, std::span<const CBlockHeader> headers)
    {
        std::unique_lock lock{m_mutex};
        if (!m_batches || !m_batches->contains(entry, false)) return false;
        // Repopulate ordinary proof evidence for subsequent AcceptBlockHeader
        // checks. The caller must first recheck every current target limit.
        for (const auto& header : headers) m_valid->insert(Entry(header));
        return true;
    }

    void InsertBatch(const uint256& entry)
    {
        std::unique_lock lock{m_mutex};
        if (m_batches) m_batches->insert(entry);
    }
};

YespowerVerificationCache& VerificationCache()
{
    static YespowerVerificationCache cache;
    return cache;
}

struct HeaderPoWCheck {
    const CBlockHeader* header;
    const Consensus::Params* params;

    // Preserve allocation/computation errors on the calling thread; do not
    // turn a local resource failure into an invalid-header/peer penalty.
    std::optional<std::exception_ptr> operator()() const
    {
        try {
            if (CheckBlockProofOfWork(*header, *params)) return std::nullopt;
            return std::exception_ptr{};
        } catch (...) {
            return std::current_exception();
        }
    }
};
} // namespace

void InitYespowerVerificationCache(size_t bytes)
{
    if (!bytes || bytes > MAX_YESPOWER_CACHE_BYTES) {
        throw std::invalid_argument("Yespower cache size must be positive and at most 2048 MiB");
    }
    VerificationCache().Reset(bytes);
}

// Official Sugarchain SugarShield: average 510 targets, using endpoint MTPs.
// Preserve integer operation ordering (divide before multiply) for consensus.
static unsigned int GetSugarShieldWorkRequired(const CBlockIndex* last, const Consensus::Params& params)
{
    const arith_uint256 limit = UintToArith256(params.powLimit);
    const CBlockIndex* first = last;
    arith_uint256 total{0};
    for (int64_t i = 0; first && i < params.nPowAveragingWindow; ++i) {
        arith_uint256 target;
        target.SetCompact(first->nBits);
        total += target;
        first = first->pprev;
    }
    if (!first) return limit.GetCompact();
    int64_t span = last->GetMedianTimePast() - first->GetMedianTimePast();
    span = params.AveragingWindowTimespan() + (span - params.AveragingWindowTimespan()) / 4;
    span = std::clamp(span, params.MinActualTimespan(), params.MaxActualTimespan());
    arith_uint256 next = total / params.nPowAveragingWindow;
    next /= params.AveragingWindowTimespan();
    next *= span;
    if (next > limit) next = limit;
    return next.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    if (params.nPowAveragingWindow) return GetSugarShieldWorkRequired(pindexLast, params);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then it MUST be a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4
    if (params.enforce_BIP94) {
        // Here we use the first block of the difficulty period. This way
        // the real difficulty is always preserved in the first block as
        // it is not allowed to use the min-difficulty exception.
        int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
        const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

// Always validate the current network's target limit, including on cache hits.
bool CheckBlockProofOfWork(const CBlockHeader& header, const Consensus::Params& params)
{
    if (!DeriveTarget(header.nBits, params.powLimit)) return false;
    if (!params.fYespowerSugar || EnableFuzzDeterminism()) {
        return CheckProofOfWork(params.fYespowerSugar ? header.GetPoWHash() : header.GetHash(), header.nBits, params);
    }
    auto& cache{VerificationCache()};
    const auto entry{cache.Entry(header)};
    if (cache.Contains(entry)) return true;
    if (!CheckProofOfWork(header.GetPoWHash(), header.nBits, params)) return false;
    cache.Insert(entry);
    return true;
}

void CacheVerifiedBlockIndexProof(const CBlockIndex& index, const Consensus::Params& params)
{
    AssertLockHeld(cs_main);
    if (!params.fYespowerSugar || EnableFuzzDeterminism() || !index.m_checked_yespower || !index.phashBlock) return;
    const auto header{index.GetBlockHeader()};
    if (header.GetHash() != index.GetBlockHash() || !DeriveTarget(header.nBits, params.powLimit)) return;
    auto& cache{VerificationCache()};
    cache.Insert(cache.Entry(header));
}

struct HeaderPoWVerifier::Impl {
    const int workers;
    std::mutex caller_mutex;
    CCheckQueue<HeaderPoWCheck> queue;
    explicit Impl(int count)
        : workers{std::clamp(count, 1, MAX_HEADER_POW_WORKERS)}, queue{1, workers - 1, "powch"} {}
};

HeaderPoWVerifier::HeaderPoWVerifier(int workers)
    : m_impl{workers > 1 ? std::make_unique<Impl>(workers) : nullptr} {}
HeaderPoWVerifier::~HeaderPoWVerifier() = default;

bool HeaderPoWVerifier::Check(std::span<const CBlockHeader> headers, const Consensus::Params& params)
{
    if (!params.fYespowerSugar || EnableFuzzDeterminism()) {
        return std::ranges::all_of(headers, [&](const auto& header) { return CheckBlockProofOfWork(header, params); });
    }
    std::unique_lock<std::mutex> call_lock;
    if (m_impl) call_lock = std::unique_lock{m_impl->caller_mutex};
    if (headers.empty()) return true;
    // An invalid first header must not cause speculative work for the message.
    if (!CheckBlockProofOfWork(headers.front(), params)) return false;
    if (headers.size() == 1) return true;
    auto& cache{VerificationCache()};
    bool all_cached{true};
    for (const auto& header : headers) {
        if (!DeriveTarget(header.nBits, params.powLimit)) return false;
        all_cached = all_cached && cache.Contains(cache.Entry(header));
    }
    if (all_cached) return true; // Preserve the ordinary warm-cache fast path.
    const auto entry{cache.BatchEntry(headers)};
    if (cache.RestoreBatch(entry, headers)) return true;
    headers = headers.subspan(1);
    if (!m_impl) {
        if (!std::ranges::all_of(headers, [&](const auto& header) { return CheckBlockProofOfWork(header, params); })) return false;
        cache.InsertBatch(entry);
        return true;
    }
    while (!headers.empty()) {
        std::vector<HeaderPoWCheck> batch;
        batch.reserve(m_impl->workers);
        while (!headers.empty() && batch.size() < size_t(m_impl->workers)) {
            const auto& header{headers.front()};
            if (!cache.Contains(cache.Entry(header))) batch.push_back({&header, &params});
            headers = headers.subspan(1);
        }
        if (batch.empty()) continue;
        // At most eight cold checks may run before the next failure decision,
        // independently of message/peer count. Never enqueue the whole message.
        CCheckQueueControl<HeaderPoWCheck> control{m_impl->queue};
        control.Add(std::move(batch));
        if (const auto error{control.Complete()}) {
            if (*error) std::rethrow_exception(*error);
            return false;
        }
    }
    cache.InsertBatch(entry);
    return true;
}
