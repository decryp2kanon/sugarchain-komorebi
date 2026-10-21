// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#ifndef BITCOIN_TEST_HISTORICAL_POW_CRASH_H
#define BITCOIN_TEST_HISTORICAL_POW_CRASH_H

#include <cstddef>
#include <cstdint>

// Test-executable-only tracing. A no-op outside an explicitly armed crash child.
void HistoricalPoWCrashProof(const uint8_t* input, size_t size, int result);

#endif // BITCOIN_TEST_HISTORICAL_POW_CRASH_H
