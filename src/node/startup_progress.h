// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_STARTUP_PROGRESS_H
#define BITCOIN_NODE_STARTUP_PROGRESS_H

#include <node/interface_ui.h>
#include <tinyformat.h>
#include <util/time.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace node {
/** Startup-only progress. Clocks and notifications are evaluated at percent boundaries,
 * never for each entry. Sorting has a known size but no measurable percent progress. */
class StartupProgress
{
    const std::string m_stage;
    const uint64_t m_total;
    const SteadyClock::time_point m_start{SteadyClock::now()};
    uint64_t m_processed{0};
    unsigned m_next_percent{1};
    uint64_t m_next_count{Threshold(1)};

    uint64_t Threshold(unsigned percent) const
    {
        return (m_total / 100) * percent + ((m_total % 100) * percent + 99) / 100;
    }
    static std::string Group(uint64_t count)
    {
        std::string result{std::to_string(count)};
        for (size_t pos = result.size(); pos > 3; pos -= 3) result.insert(pos - 3, ",");
        return result;
    }
    int64_t Elapsed() const
    {
        return std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - m_start).count();
    }

public:
    StartupProgress(const std::string& stage, uint64_t total) : m_stage{stage}, m_total{total}
    {
        uiInterface.InitMessage(strprintf("%s... | %s entries | elapsed 0s", m_stage, Group(m_total)));
    }
    void Advance()
    {
        ++m_processed;
        if (m_processed < m_next_count || m_processed >= m_total || m_next_percent >= 100) return;
        unsigned percent{m_next_percent};
        while (percent < 99 && m_processed >= Threshold(percent + 1)) ++percent;
        Report(percent);
        m_next_percent = percent + 1;
        m_next_count = Threshold(m_next_percent);
    }
    void Report(unsigned percent) const
    {
        const int64_t elapsed{Elapsed()};
        const std::string eta{percent == 100 ? "0s" : elapsed == 0 ? "--" :
            strprintf("%ds", static_cast<int64_t>((static_cast<double>(elapsed) * (m_total - m_processed) / m_processed) + 0.999))};
        uiInterface.InitMessage(strprintf("%s: %s / %s (%u%%) | elapsed %ds | ETA %s",
            m_stage, Group(m_processed), Group(m_total), percent, elapsed, eta));
    }
    /** Only call after successful completion; interrupted/failed work must never report 100%. */
    void Finish() const
    {
        if (m_processed == m_total) Report(100);
    }
    /** Sorting is opaque: publish only start and finish, never a fabricated percentage. */
    void FinishSort() const
    {
        uiInterface.InitMessage(strprintf("%s completed: %s entries | elapsed %ds", m_stage, Group(m_total), Elapsed()));
    }
};
} // namespace node

#endif // BITCOIN_NODE_STARTUP_PROGRESS_H
