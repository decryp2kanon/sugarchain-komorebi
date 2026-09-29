// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Offline safety diagnostic only: link a separate daemon with --wrap=yespower.
// Always call the real implementation; record inputs, never substitute results.
#include <crypto/common.h>
#include <crypto/yespower-1.0.1/yespower.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {
struct Trace {
    std::mutex mutex;
    std::map<std::array<uint8_t, 80>, uint64_t> inputs;
    uint64_t calls{0}, failures{0};
    bool parameters_ok{true}, overflow{false};

    ~Trace()
    {
        const char* path{std::getenv("YESPOWER_PROOF_TRACE")};
        FILE* file{path ? std::fopen(path, "wbx") : nullptr};
        bool written{file != nullptr};
        if (file) {
            written = std::fwrite("YPT1", 1, 4, file) == 4;
            for (const auto& [input, count] : inputs) {
                unsigned char encoded[8];
                WriteLE64(encoded, count);
                written &= std::fwrite(input.data(), 1, input.size(), file) == input.size();
                written &= std::fwrite(encoded, 1, sizeof(encoded), file) == sizeof(encoded);
            }
            written &= std::fclose(file) == 0;
        }
        std::fprintf(stderr, "YESPOWER_TRACE {\"calls\":%llu,\"unique\":%zu,\"failures\":%llu,\"parameters_ok\":%s,\"overflow\":%s,\"written\":%s}\n",
                     static_cast<unsigned long long>(calls), inputs.size(), static_cast<unsigned long long>(failures),
                     parameters_ok ? "true" : "false", overflow ? "true" : "false", written ? "true" : "false");
    }
};
Trace& GetTrace() { static Trace trace; return trace; }
}

extern "C" int __real_yespower(yespower_local_t*, const uint8_t*, size_t, const yespower_params_t*, yespower_binary_t*);
extern "C" int __wrap_yespower(yespower_local_t* local, const uint8_t* input, size_t size,
                              const yespower_params_t* params, yespower_binary_t* output)
{
    const int result{__real_yespower(local, input, size, params, output)};
    auto& trace{GetTrace()};
    std::lock_guard lock{trace.mutex};
    ++trace.calls;
    if (result) ++trace.failures;
    static constexpr char personal[]{"Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote"};
    trace.parameters_ok &= size == 80 && params->version == YESPOWER_1_0 && params->N == 2048 && params->r == 32 &&
        params->perslen == sizeof(personal) - 1 && params->pers && std::memcmp(params->pers, personal, sizeof(personal) - 1) == 0;
    if (size == 80) {
        std::array<uint8_t, 80> key;
        std::copy_n(input, key.size(), key.begin());
        const auto found{trace.inputs.find(key)};
        if (found != trace.inputs.end()) ++found->second;
        else if (trace.inputs.size() < 100010) trace.inputs.emplace(key, 1);
        else trace.overflow = true; // Bound diagnostic memory even if misused.
    }
    return result;
}
