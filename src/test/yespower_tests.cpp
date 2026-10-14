// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <crypto/yespower-1.0.1/yespower.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace {
constexpr uint8_t SUGAR_PERSONALIZATION[]{"Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote"};
constexpr std::array<uint8_t, 7> BINARY_PERSONALIZATION{0, 255, 128, 17, 0, 200, 3};
constexpr auto CHANGED_SUGAR_PERSONALIZATION = [] {
    std::array<uint8_t, sizeof(SUGAR_PERSONALIZATION) - 1> p{};
    std::copy_n(SUGAR_PERSONALIZATION, p.size(), p.begin());
    p[0] ^= 1;
    return p;
}();
struct TestVector {
    yespower_version_t version;
    uint32_t n, r;
    size_t length;
    unsigned personalization;
    std::string_view expected;
};
// Golden raw digest bytes come from the separately built, unchanged yespower
// implementation at 49b9f7ed78cd37e5eae1f28e4ed24e8cc89d76a4. The input is
// byte[i] = (37*i + 19) mod 256. Personalization 0: absent; 1: SUGAR above;
// 2: BINARY above; 3: non-null BINARY pointer with length zero; 4: SUGAR with
// exactly its first byte changed; 5: SUGAR with its last byte omitted. Digests
// are printed in byte order, not uint256 display order. Vector 1 is the exact
// node's 80-byte length/parameter combination. Vectors 5 and 8-13 change only
// version, N, r, input length, personalization content, personalization length,
// or personalization presence, respectively. The other vectors cover edges.
constexpr std::array<TestVector, 14> VECTORS{{
    {YESPOWER_1_0, 2048, 32, 0, 0, "5b6f58070b31ec7afa27101fe63d870568705892fe217550a9a3c10b241354ba"},
    {YESPOWER_1_0, 2048, 32, 80, 1, "b812ca51d8c2f84ef632466f6136e06524cffecb2731be9f85623b62b3d28e8a"},
    {YESPOWER_1_0, 2048, 32, 81, 2, "6b4a4b0c9248b6a135f2213b2bb751c1768ee952607e17acc4b9d5deaad318c7"},
    {YESPOWER_1_0, 2048, 16, 80, 2, "39ae0b6c1e23a2b6aa1abc8c65056a7f4f563fff1e686b60df7fb77b3d1a305d"},
    {YESPOWER_1_0, 1024, 32, 81, 0, "241161153bbc03700970ecf65f94f5f9ede795f4f36f74f7133e36a464abdd41"},
    {YESPOWER_0_5, 2048, 32, 80, 1, "67ee8983ce1ef5c8724db7e527c01d914dd0c5b34d1ef3f78066561619b91881"},
    {YESPOWER_0_5, 1024, 16, 0, 0, "964d410a20aa6e837589be10f41264097106cbe25e1e9cf7fff29ba3afc87e05"},
    {YESPOWER_0_5, 1024, 16, 81, 3, "d5e6d48433c1b439b70e997c390e77456e7e840db3dd384ef755f64a7f8658c5"},
    {YESPOWER_1_0, 1024, 32, 80, 1, "d4d618f5ad4a1792ff9a9c5351c503bf481447c8802bfce67d33acb45bc11fc2"},
    {YESPOWER_1_0, 2048, 16, 80, 1, "78cdc446cb6a1f828072550b97704e4e613db7167fc8318cf7a5adb926c4a66e"},
    {YESPOWER_1_0, 2048, 32, 81, 1, "7028da07715275d4397ac3edc29dee87488c84f1266034d2f40871e33a7a23d0"},
    {YESPOWER_1_0, 2048, 32, 80, 4, "720be59fb8a2c0f4bf03c5824b911d613b44c78d9e30750b65637d8b7057a821"},
    {YESPOWER_1_0, 2048, 32, 80, 5, "e59148759281f5e304fe770f45115f6ccbed07a0763456949e6e6acbbb687b13"},
    {YESPOWER_1_0, 2048, 32, 80, 0, "8f2cd644d29882667f912ad93ef59dc3a7e82dc2335d48337b7901c572e04fac"},
}};
yespower_params_t Parameters(const TestVector& v)
{
    const uint8_t* personal{nullptr};
    size_t length{0};
    switch (v.personalization) {
    case 1: personal = SUGAR_PERSONALIZATION; length = sizeof(SUGAR_PERSONALIZATION) - 1; break;
    case 2: personal = BINARY_PERSONALIZATION.data(); length = BINARY_PERSONALIZATION.size(); break;
    case 3: personal = BINARY_PERSONALIZATION.data(); break;
    case 4: personal = CHANGED_SUGAR_PERSONALIZATION.data(); length = CHANGED_SUGAR_PERSONALIZATION.size(); break;
    case 5: personal = SUGAR_PERSONALIZATION; length = sizeof(SUGAR_PERSONALIZATION) - 2; break;
    }
    return {v.version, v.n, v.r, personal, length};
}
struct Local {
    yespower_local_t value{};
    Local() { BOOST_REQUIRE_EQUAL(yespower_init_local(&value), 0); }
    ~Local() { yespower_free_local(&value); }
};
std::array<uint8_t, 82> Input()
{
    std::array<uint8_t, 82> input{};
    // Leave one leading byte to exercise an unaligned source pointer.
    for (size_t i{0}; i < 81; ++i) input[i + 1] = uint8_t(i * 37 + 19);
    return input;
}
void CheckVector(Local& local, size_t index)
{
    alignas(64) const auto input{Input()};
    const auto& v{VECTORS.at(index)};
    const auto params{Parameters(v)};
    yespower_binary_t output{};
    BOOST_REQUIRE_EQUAL(yespower(&local.value, input.data() + 1, v.length, &params, &output), 0);
    BOOST_CHECK_EQUAL(HexStr(std::span{output.uc}), std::string{v.expected});
}
} // namespace

BOOST_AUTO_TEST_SUITE(yespower_tests)

BOOST_AUTO_TEST_CASE(known_outputs_and_generic_fallbacks)
{
    Local local;
    for (size_t i{0}; i < VECTORS.size(); ++i) {
        BOOST_TEST_CONTEXT("Yespower vector " << i) { CheckVector(local, i); }
    }
}

BOOST_AUTO_TEST_CASE(invalid_parameters_preserve_scratch_and_report_failure)
{
    Local local;
    CheckVector(local, 1);
    const auto before{local.value};
    alignas(64) const auto input{Input()};
    const std::array<yespower_params_t, 6> invalid{{
        {YESPOWER_1_0, 0, 32, nullptr, 0},
        {YESPOWER_1_0, 1025, 32, nullptr, 0},
        {YESPOWER_1_0, 2048, 7, nullptr, 0},
        {YESPOWER_1_0, 2048, 33, nullptr, 0},
        {static_cast<yespower_version_t>(0), 2048, 32, nullptr, 0},
        {YESPOWER_1_0, 2048, 32, nullptr, 1},
    }};
    for (const auto& params : invalid) {
        yespower_binary_t output{};
        errno = 0;
        const int result{yespower(&local.value, input.data() + 1, 80, &params, &output)};
        const int error{errno};
        BOOST_CHECK_EQUAL(result, -1);
        BOOST_CHECK_EQUAL(error, EINVAL);
        BOOST_CHECK(std::all_of(std::begin(output.uc), std::end(output.uc), [](uint8_t x) { return x == 0xff; }));
        BOOST_CHECK(local.value.base == before.base);
        BOOST_CHECK(local.value.aligned == before.aligned);
        BOOST_CHECK_EQUAL(local.value.base_size, before.base_size);
        BOOST_CHECK_EQUAL(local.value.aligned_size, before.aligned_size);
    }
    CheckVector(local, 1); // Invalid input must not poison the next valid call.
}

BOOST_AUTO_TEST_CASE(scratch_reuse_free_and_reinitialize)
{
    Local local;
    CheckVector(local, 1);
    const auto original{local.value};
    CheckVector(local, 1);
    CheckVector(local, 6); // Smaller generic 0.5 call reuses the existing region.
    CheckVector(local, 1);
    BOOST_CHECK(local.value.base == original.base);
    BOOST_CHECK(local.value.aligned == original.aligned);
    BOOST_CHECK_EQUAL(local.value.aligned_size, original.aligned_size);
    BOOST_REQUIRE_EQUAL(yespower_free_local(&local.value), 0);
    BOOST_CHECK(local.value.base == nullptr);
    BOOST_CHECK(local.value.aligned == nullptr);
    BOOST_CHECK_EQUAL(local.value.base_size, 0U);
    BOOST_CHECK_EQUAL(local.value.aligned_size, 0U);
    BOOST_REQUIRE_EQUAL(yespower_init_local(&local.value), 0);
    CheckVector(local, 1);
}

BOOST_AUTO_TEST_SUITE_END()
