// Copyright (c) 2026 The Sugarchain Komorebi developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <test/historical_pow_crash.h>

#include <kernel/chainparams.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <streams.h>
#include <test/data/sugarchain_headers.raw.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#ifdef ENABLE_YESPOWER_TEST_WRAP
#include <csignal>
#include <unistd.h>

namespace {
std::atomic<bool> armed{false};
std::atomic<unsigned> proofs{0}, writes{0};
std::mutex event_mutex;
std::string point;
thread_local unsigned writing{0};

void Event(UniValue value)
{
    std::lock_guard lock{event_mutex};
    std::cout << "CRASH_EVENT " << value.write() << std::endl;
}

void Boundary(const std::string& name, unsigned ordinal)
{
    UniValue event{UniValue::VOBJ};
    event.pushKV("event", name);
    event.pushKV("ordinal", ordinal);
    Event(std::move(event));
    if (point == name + ":" + std::to_string(ordinal)) {
        UniValue stopped{UniValue::VOBJ};
        stopped.pushKV("event", "kill_ready");
        stopped.pushKV("point", point);
        Event(std::move(stopped));
        // Stop all threads; the supervising parent sends SIGKILL and checks it.
        raise(SIGSTOP);
        std::abort(); // Never proceed if somebody unexpectedly resumes the child.
    }
}

bool IsWAL(int fd)
{
    char path[4096];
    const auto n{readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path, sizeof(path))};
    if (n <= 0) return false;
    const std::string_view full{path, static_cast<size_t>(n)};
    const auto name{full.substr(full.find_last_of('/') + 1)};
    if (name.size() <= 4 || !name.ends_with(".log")) return false;
    for (const char c : name.substr(0, name.size() - 4)) {
        if (c < '0' || c > '9') return false;
    }
    return true; // A numeric LevelDB WAL, never the test's debug.log.
}
} // namespace
#endif

void HistoricalPoWCrashProof(const uint8_t* input, size_t size, int result)
{
#ifdef ENABLE_YESPOWER_TEST_WRAP
    if (!armed.load()) return;
    DataStream stream{std::span{input, size}};
    CBlockHeader header;
    stream >> header;
    UniValue event{UniValue::VOBJ};
    event.pushKV("event", "proof");
    event.pushKV("hash", header.GetHash().ToString());
    event.pushKV("result", result);
    Event(std::move(event));
    Boundary("proof_return", ++proofs);
#endif
}

#ifdef ENABLE_YESPOWER_TEST_WRAP
extern "C" void __real__ZN10CDBWrapper10WriteBatchER8CDBBatchb(CDBWrapper*, CDBBatch&, bool);
extern "C" void __wrap__ZN10CDBWrapper10WriteBatchER8CDBBatchb(CDBWrapper* db, CDBBatch& batch, bool sync)
{
    if (!armed.load() || !sync) {
        __real__ZN10CDBWrapper10WriteBatchER8CDBBatchb(db, batch, sync);
        return;
    }
    const auto ordinal{++writes};
    Boundary("write_begin", ordinal);
    writing = ordinal;
    __real__ZN10CDBWrapper10WriteBatchER8CDBBatchb(db, batch, sync);
    writing = 0;
    Boundary("write_committed", ordinal);
}

extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd)
{
    if (!armed.load() || !writing) return __real_fdatasync(fd);
    // Only the WAL sync belonging to this synchronous evidence write, not
    // manifest/directory sync or database setup. Never replace the real sync.
    const bool wal{IsWAL(fd)};
    if (wal) Boundary("wal_sync_before", writing);
    const int result{__real_fdatasync(fd)};
    if (wal && result == 0) Boundary("wal_sync_after", writing);
    return result;
}

extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    if (armed.load() && writing == 2 && point == "wal_partial:2" && size > 1 && IsWAL(fd)) {
        // Persist a real prefix, not a fabricated successful write. Stop before
        // LevelDB can finish this record or acknowledge the evidence batch.
        const auto result{__real_write(fd, data, size / 2)};
        if (result > 0) {
            UniValue event{UniValue::VOBJ};
            event.pushKV("event", "partial_wal_write");
            event.pushKV("requested", static_cast<uint64_t>(size));
            event.pushKV("written", static_cast<int64_t>(result));
            Event(std::move(event));
            Boundary("wal_partial", writing);
        }
        return result;
    }
    return __real_write(fd, data, size);
}
#endif

BOOST_FIXTURE_TEST_SUITE(historical_pow_crash_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(child)
{
#ifdef ENABLE_YESPOWER_TEST_WRAP
    const char* control{std::getenv("SUGAR_LAZY_CRASH_CONTROL")};
    if (!control) return; // Driven through lazy_pow_crash_recovery.py / CTest.
    std::ifstream file{control};
    UniValue config;
    BOOST_REQUIRE(config.read(std::string{std::istreambuf_iterator<char>{file}, {}}));
    const auto root{fs::PathFromString(config["root"].get_str())};
    BOOST_REQUIRE(fs::exists(root / ".lazy-pow-crash-test"));
    BOOST_REQUIRE(!fs::exists(root / "blocks"));
    const std::string mode{config["mode"].get_str()};
    const bool expect_error{config.exists("expect_error") && config["expect_error"].get_bool()};
    point = config["point"].get_str();
    const int workers{config["workers"].getInt<int>()};
    BOOST_REQUIRE(workers == 1 || workers == 8);
    const auto chain{CChainParams::Main()};
    const auto& params{chain->GetConsensus()};
    DataStream stream{test::data::sugarchain_headers};
    std::vector<CBlockHeader> headers(6000);
    for (auto& header : headers) stream >> header;
    kernel::BlockTreeDB db{DBParams{.path = root / "index", .cache_bytes = 1 << 20}};
    if (mode == "seed") {
        HeaderPoWVerifier verifier{8};
        BOOST_REQUIRE(verifier.CheckEach(headers, params, false));
        const auto genesis_hash{chain->GenesisBlock().GetHash()};
        CBlockIndex genesis{chain->GenesisBlock()};
        genesis.phashBlock = &genesis_hash;
        std::deque<CBlockIndex> indexes;
        std::vector<uint256> hashes;
        std::vector<const CBlockIndex*> entries;
        hashes.reserve(headers.size());
        CBlockIndex* previous{&genesis};
        for (const auto& header : headers) {
            BOOST_REQUIRE(header.hashPrevBlock == previous->GetBlockHash());
            hashes.push_back(header.GetHash());
            indexes.emplace_back(header);
            auto& index{indexes.back()};
            index.phashBlock = &hashes.back();
            index.pprev = previous;
            index.nHeight = previous->nHeight + 1;
            index.nStatus = BLOCK_VALID_TREE;
            previous = &index;
            entries.push_back(&index);
        }
        db.WriteBatchSync({}, 0, entries);
    }
    if (mode == "damage") {
        db.Write(std::make_pair(uint8_t{'y'}, headers[0].GetHash()), uint8_t{2}, true);
        db.Write(std::make_pair(uint8_t{'y'}, headers[1].GetHash()),
                 std::make_pair(uint8_t{99}, GetYespowerEvidenceDigest(headers[1], params)), true);
        db.Write(std::make_pair(uint8_t{'y'}, headers[2].GetHash()),
                 std::make_pair(uint8_t{2}, uint256{}), true);
        db.Erase(std::make_pair(uint8_t{'y'}, headers[3].GetHash()), true);
    }
    if (mode == "bad_pow" || mode == "bad_identity") {
        auto bad{headers[7]};
        do { ++bad.nNonce; } while (CheckProofOfWorkImpl(bad.GetPoWHash(), bad.nBits, params));
        CDiskBlockIndex disk;
        BOOST_REQUIRE(db.Read(std::make_pair(uint8_t{'b'}, headers[7].GetHash()), disk));
        disk.nNonce = bad.nNonce;
        const auto key{mode == "bad_pow" ? bad.GetHash() : headers[7].GetHash()};
        db.Write(std::make_pair(uint8_t{'b'}, key), disk, true);
        BOOST_CHECK(!db.HasYespowerEvidence(bad, params));
        UniValue event{UniValue::VOBJ};
        event.pushKV("event", "invalid_header");
        event.pushKV("hash", key.ToString());
        Event(std::move(event));
    }
    UniValue inventory{UniValue::VOBJ}, present{UniValue::VARR}, expected{UniValue::VARR};
    for (const auto& header : headers) {
        const auto hash{header.GetHash()};
        expected.push_back(hash.ToString());
        if (db.HasYespowerEvidence(header, params)) present.push_back(hash.ToString());
    }
    inventory.pushKV("event", "inventory");
    inventory.pushKV("expected", std::move(expected));
    inventory.pushKV("present", std::move(present));
    Event(std::move(inventory));
    if (mode == "seed" || mode == "inspect" || mode == "damage" || mode == "bad_pow" || mode == "bad_identity") return;
    BOOST_REQUIRE(mode == "crash" || mode == "resume");
    InitYespowerVerificationCache(1 << 20);
    util::SignalInterrupt interrupt;
    kernel::HistoricalPoWStats stats;
    stats.total = headers.size();
    Boundary("scan_start", 0);
    armed = true;
    const auto error{db.VerifyHistoricalPoW(params, interrupt, stats, 0, [&](const CBlockHeader&) {
        BOOST_CHECK(stats.state == kernel::HistoricalPoWStats::RUNNING);
        return false;
    }, workers)};
    armed = false;
    UniValue result{UniValue::VOBJ};
    result.pushKV("event", "scan_result");
    result.pushKV("complete", stats.state == kernel::HistoricalPoWStats::COMPLETE);
    result.pushKV("processed", stats.processed.load());
    result.pushKV("verified", stats.verified.load());
    result.pushKV("reused", stats.reused.load());
    result.pushKV("error", error.value_or(""));
    Event(std::move(result));
    if (expect_error) {
        BOOST_REQUIRE(error);
        BOOST_CHECK(stats.state == kernel::HistoricalPoWStats::FAILED);
        return;
    }
    BOOST_REQUIRE(!error);
    BOOST_CHECK(stats.state == kernel::HistoricalPoWStats::COMPLETE);
    BOOST_CHECK_EQUAL(stats.processed.load(), headers.size());
    for (const auto& header : headers) BOOST_CHECK(db.HasYespowerEvidence(header, params));
#endif
}

BOOST_AUTO_TEST_SUITE_END()
