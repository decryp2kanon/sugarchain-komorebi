// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_TEST_UTIL_YESPOWER_OBSERVER_H
#define BITCOIN_TEST_UTIL_YESPOWER_OBSERVER_H

namespace test {
class YespowerCallCounter {
public:
    YespowerCallCounter();
    ~YespowerCallCounter();
    unsigned Calls() const;
};
} // namespace test

#endif // BITCOIN_TEST_UTIL_YESPOWER_OBSERVER_H
