// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#ifndef BITCOIN_NODE_IBD_DOWNLOAD_H
#define BITCOIN_NODE_IBD_DOWNLOAD_H

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace node {
/** Delivery-rate hint only: never changes validation, the download window or hard limits. */
class IBDBlockDelivery
{
    int64_t m_last_us{0};
    int64_t m_interval_us{0};
    int64_t m_started_us{0};
    int64_t m_latency_us{0};

public:
    void Started(int64_t now_us)
    {
        m_started_us = std::max<int64_t>(0, now_us);
        m_last_us = 0; // The previous queue may have been cancelled, not delivered.
    }

    void Received(int64_t now_us, bool more_pending)
    {
        if (m_started_us > 0) {
            if (now_us > m_started_us) m_latency_us = std::min<int64_t>(now_us - m_started_us, 60'000'000);
            m_started_us = 0;
        }
        if (m_last_us > 0 && now_us > m_last_us) {
            // Bound arithmetic and smooth bursty arrivals. Ignore nonpositive
            // deltas; mock/system-clock changes must not create infinite rates.
            const auto interval{std::min<int64_t>(now_us - m_last_us, 60'000'000)};
            m_interval_us = m_interval_us ? m_interval_us + (interval - m_interval_us) / 8 : interval;
        }
        // Do not count an idle period with no outstanding work as peer slowness.
        m_last_us = more_pending ? std::max<int64_t>(0, now_us) : 0;
    }

    int Limit(int minimum, int maximum) const
    {
        assert(minimum > 0 && maximum >= minimum);
        // Cover first-response latency plus 250ms of observed service work.
        // Both the upstream floor and the configured resource ceiling remain.
        // Before measurements, preserve the existing configured request budget.
        if (!m_interval_us) return maximum;
        return std::clamp<int64_t>((m_latency_us + 250'000) / m_interval_us, minimum, maximum);
    }
};
} // namespace node

#endif // BITCOIN_NODE_IBD_DOWNLOAD_H
