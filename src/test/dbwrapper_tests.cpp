// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/string.h>

#include <leveldb/db.h>
#include <leveldb/options.h>

#include <memory>
#include <ranges>

#include <boost/test/unit_test.hpp>

using util::ToString;

BOOST_FIXTURE_TEST_SUITE(dbwrapper_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(block_index_key_prefix_preserves_parser_boundaries)
{
    for (const bool obfuscate : {false, true}) {
        CDBWrapper db{{.path = m_args.GetDataDirBase() / "key-prefix", .cache_bytes = 1_MiB, .memory_only = true, .obfuscate = obfuscate}};
        for (size_t length = 0; length <= 40; ++length) {
            std::vector<std::byte> bytes(length, std::byte{0x11});
            if (!bytes.empty()) bytes.front() = std::byte{'b'};
            db.Write(std::span<const std::byte>{bytes}, uint8_t{1});
        }
        for (const uint8_t prefix : {uint8_t{'a'}, uint8_t{'c'}}) {
            std::vector<std::byte> bytes(33, std::byte{0x11});
            bytes.front() = std::byte{prefix};
            db.Write(std::span<const std::byte>{bytes}, uint8_t{1});
        }
        auto cursor{std::unique_ptr<CDBIterator>{db.NewIterator()}};
        size_t matched{0};
        for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
            std::pair<uint8_t, uint256> parsed;
            const bool decoded{cursor->GetKey(parsed) && parsed.first == uint8_t{'b'}};
            const bool prefixed{cursor->KeyHasPrefix(uint8_t{'b'}, 1 + uint256::size())};
            BOOST_CHECK_EQUAL(prefixed, decoded);
            matched += prefixed;
        }
        BOOST_CHECK_EQUAL(matched, 8U); // Lengths 33-40; trailing bytes were already accepted.
    }
}

BOOST_AUTO_TEST_CASE(small_table_cache_preserves_reads_across_eviction)
{
    const std::string path{fs::PathToString(m_args.GetDataDirBase() / "small-table-cache")};
    constexpr uint32_t ROWS{4096};
    auto key = [](uint32_t i) { return strprintf("%08u", i); };
    auto value = [&key](uint32_t i) { return key(i) + std::string(8192, char(i % 251)); };
    leveldb::Options options;
    options.create_if_missing = true;
    options.small_table_cache = true;
    options.max_open_files = 26;
    options.write_buffer_size = 64 * 1024;
    options.max_file_size = 1024 * 1024;
    options.compression = leveldb::kNoCompression;
    {
        leveldb::DB* raw{nullptr};
        BOOST_REQUIRE(leveldb::DB::Open(options, path, &raw).ok());
        const std::unique_ptr<leveldb::DB> db{raw};
        for (uint32_t i = 0; i < ROWS; ++i) {
            BOOST_REQUIRE(db->Put(leveldb::WriteOptions{}, key(i), value(i)).ok());
        }
        db->CompactRange(nullptr, nullptr);
        size_t tables{0};
        for (int level = 0; level < 7; ++level) {
            std::string count;
            BOOST_REQUIRE(db->GetProperty(strprintf("leveldb.num-files-at-level%d", level), &count));
            tables += std::stoul(count);
        }
        BOOST_REQUIRE_GT(tables, 16U); // Force reads across more tables than the cache can retain.
    }
    for (const bool small : {false, true}) {
        options.small_table_cache = small;
        leveldb::DB* raw{nullptr};
        BOOST_REQUIRE(leveldb::DB::Open(options, path, &raw).ok());
        const std::unique_ptr<leveldb::DB> db{raw};
        for (uint32_t i = 0; i < ROWS; ++i) {
            for (const uint32_t row : {i, ROWS - 1 - i}) {
                std::string actual;
                BOOST_REQUIRE(db->Get(leveldb::ReadOptions{}, key(row), &actual).ok());
                BOOST_CHECK_EQUAL(actual, value(row));
            }
        }
        const std::unique_ptr<leveldb::Iterator> cursor{db->NewIterator(leveldb::ReadOptions{})};
        uint32_t row{0};
        for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next(), ++row) {
            BOOST_REQUIRE_LT(row, ROWS);
            BOOST_CHECK_EQUAL(cursor->key().ToString(), key(row));
            BOOST_CHECK_EQUAL(cursor->value().ToString(), value(row));
        }
        BOOST_CHECK(cursor->status().ok());
        BOOST_CHECK_EQUAL(row, ROWS);
    }
}

BOOST_AUTO_TEST_CASE(read_cache_reclamation_preserves_db_contents)
{
    const fs::path path{m_args.GetDataDirBase() / "read-cache-reclaim"};
    for (const bool obfuscate : {false, true}) {
        {
            CDBWrapper db{{.path = path, .cache_bytes = 1_MiB, .wipe_data = true, .obfuscate = obfuscate,
                           .reclaim_read_cache = true}};
            CDBBatch batch{db};
            for (uint32_t i = 0; i < 4096; ++i) batch.Write(i, uint64_t{i} * 17);
            db.WriteBatch(batch, true);
            db.CompactFull();
        }
        for (const bool reclaim : {false, true}) {
            CDBWrapper db{{.path = path, .cache_bytes = 1_MiB, .reclaim_read_cache = reclaim}};
            for (uint32_t i = 0; i < 4096; ++i) {
                uint64_t value{0};
                BOOST_REQUIRE(db.Read(i, value));
                BOOST_CHECK_EQUAL(value, uint64_t{i} * 17);
            }
            const std::unique_ptr<CDBIterator> cursor{db.NewIterator()};
            size_t count{0};
            for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
                uint32_t key{0};
                if (!cursor->GetKey(key) || key >= 4096) continue; // Internal obfuscation metadata.
                uint64_t value{0};
                BOOST_REQUIRE(cursor->GetValue(value));
                BOOST_CHECK_EQUAL(value, uint64_t{key} * 17);
                ++count;
            }
            BOOST_CHECK_EQUAL(count, 4096U);
        }
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        constexpr size_t CACHE_SIZE{1_MiB};
        const fs::path path{m_args.GetDataDirBase() / "dbwrapper"};

        Obfuscation obfuscation;
        std::vector<std::pair<uint8_t, uint256>> key_values{};

        // Write values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true, .obfuscate = obfuscate}};
            BOOST_CHECK_EQUAL(obfuscate, !dbw.IsEmpty());

            // Ensure that we're doing real obfuscation when obfuscate=true
            obfuscation = dbwrapper_private::GetObfuscation(dbw);
            BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscation(dbw));

            for (uint8_t k{0}; k < 10; ++k) {
                uint8_t key{k};
                uint256 value{m_rng.rand256()};
                dbw.Write(key, value);
                key_values.emplace_back(key, value);
            }
        }

        // Verify that the obfuscation key is never obfuscated
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = false}};
            BOOST_CHECK_EQUAL(obfuscation, dbwrapper_private::GetObfuscation(dbw));
        }

        // Read back the values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = obfuscate}};

            // Ensure obfuscation is read back correctly
            BOOST_CHECK_EQUAL(obfuscation, dbwrapper_private::GetObfuscation(dbw));
            BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscation(dbw));

            // Verify all written values
            for (const auto& [key, expected_value] : key_values) {
                uint256 read_value{};
                BOOST_CHECK(dbw.Read(key, read_value));
                BOOST_CHECK_EQUAL(read_value, expected_value);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_basic_data)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_1_obfuscate_true" : "dbwrapper_1_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = false, .wipe_data = true, .obfuscate = obfuscate});

        uint256 res;
        uint32_t res_uint_32;
        bool res_bool;

        // Ensure that we're doing real obfuscation when obfuscate=true
        BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscation(dbw));

        //Simulate block raw data - "b + block hash"
        std::string key_block = "b" + m_rng.rand256().ToString();

        uint256 in_block = m_rng.rand256();
        dbw.Write(key_block, in_block);
        BOOST_CHECK(dbw.Read(key_block, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_block.ToString());

        //Simulate file raw data - "f + file_number"
        std::string key_file = strprintf("f%04x", m_rng.rand32());

        uint256 in_file_info = m_rng.rand256();
        dbw.Write(key_file, in_file_info);
        BOOST_CHECK(dbw.Read(key_file, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_file_info.ToString());

        //Simulate transaction raw data - "t + transaction hash"
        std::string key_transaction = "t" + m_rng.rand256().ToString();

        uint256 in_transaction = m_rng.rand256();
        dbw.Write(key_transaction, in_transaction);
        BOOST_CHECK(dbw.Read(key_transaction, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_transaction.ToString());

        //Simulate UTXO raw data - "c + transaction hash"
        std::string key_utxo = "c" + m_rng.rand256().ToString();

        uint256 in_utxo = m_rng.rand256();
        dbw.Write(key_utxo, in_utxo);
        BOOST_CHECK(dbw.Read(key_utxo, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_utxo.ToString());

        //Simulate last block file number - "l"
        uint8_t key_last_blockfile_number{'l'};
        uint32_t lastblockfilenumber = m_rng.rand32();
        dbw.Write(key_last_blockfile_number, lastblockfilenumber);
        BOOST_CHECK(dbw.Read(key_last_blockfile_number, res_uint_32));
        BOOST_CHECK_EQUAL(lastblockfilenumber, res_uint_32);

        //Simulate Is Reindexing - "R"
        uint8_t key_IsReindexing{'R'};
        bool isInReindexing = m_rng.randbool();
        dbw.Write(key_IsReindexing, isInReindexing);
        BOOST_CHECK(dbw.Read(key_IsReindexing, res_bool));
        BOOST_CHECK_EQUAL(isInReindexing, res_bool);

        //Simulate last block hash up to which UXTO covers - 'B'
        uint8_t key_lastblockhash_uxto{'B'};
        uint256 lastblock_hash = m_rng.rand256();
        dbw.Write(key_lastblockhash_uxto, lastblock_hash);
        BOOST_CHECK(dbw.Read(key_lastblockhash_uxto, res));
        BOOST_CHECK_EQUAL(lastblock_hash, res);

        //Simulate file raw data - "F + filename_number + filename"
        std::string file_option_tag = "F";
        uint8_t filename_length = m_rng.randbits(8);
        std::string filename = "randomfilename";
        std::string key_file_option = strprintf("%s%01x%s", file_option_tag, filename_length, filename);

        bool in_file_bool = m_rng.randbool();
        dbw.Write(key_file_option, in_file_bool);
        BOOST_CHECK(dbw.Read(key_file_option, res_bool));
        BOOST_CHECK_EQUAL(res_bool, in_file_bool);
    }
}

// Test batch operations
BOOST_AUTO_TEST_CASE(dbwrapper_batch)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_batch_obfuscate_true" : "dbwrapper_batch_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        uint8_t key{'i'};
        uint256 in = m_rng.rand256();
        uint8_t key2{'j'};
        uint256 in2 = m_rng.rand256();
        uint8_t key3{'k'};
        uint256 in3 = m_rng.rand256();

        uint256 res;
        CDBBatch batch(dbw);

        batch.Write(key, in);
        batch.Write(key2, in2);
        batch.Write(key3, in3);

        // Remove key3 before it's even been written
        batch.Erase(key3);

        dbw.WriteBatch(batch);

        BOOST_CHECK(dbw.Read(key, res));
        BOOST_CHECK_EQUAL(res.ToString(), in.ToString());
        BOOST_CHECK(dbw.Read(key2, res));
        BOOST_CHECK_EQUAL(res.ToString(), in2.ToString());

        // key3 should've never been written
        BOOST_CHECK(dbw.Read(key3, res) == false);
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_iterator)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_iterator_obfuscate_true" : "dbwrapper_iterator_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        // The two keys are intentionally chosen for ordering
        uint8_t key{'j'};
        uint256 in = m_rng.rand256();
        dbw.Write(key, in);
        uint8_t key2{'k'};
        uint256 in2 = m_rng.rand256();
        dbw.Write(key2, in2);

        std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

        // Be sure to seek past the obfuscation key (if it exists)
        it->Seek(key);

        uint8_t key_res;
        uint256 val_res;

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key);
        BOOST_CHECK_EQUAL(val_res.ToString(), in.ToString());

        it->Next();

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key2);
        BOOST_CHECK_EQUAL(val_res.ToString(), in2.ToString());

        it->Next();
        BOOST_CHECK_EQUAL(it->Valid(), false);
    }
}

BOOST_AUTO_TEST_CASE(iterator_failed_read_does_not_consume_storage)
{
    for (const bool obfuscate : {false, true}) {
        CDBWrapper dbw({.path = m_args.GetDataDirBase() / (obfuscate ? "failed_read_obfuscated" : "failed_read_plain"),
                       .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = obfuscate});
        dbw.Write(uint8_t{'j'}, uint8_t{42});
        auto cursor{std::unique_ptr<CDBIterator>(dbw.NewIterator())};
        cursor->Seek(uint8_t{'j'});
        std::pair<uint8_t, uint256> oversized_key;
        uint256 oversized_value;
        for (const bool direct_read : {false, true}) {
            for (int repeat = 0; repeat < 2; ++repeat) {
                BOOST_CHECK(!cursor->GetKey(oversized_key, direct_read));
                BOOST_CHECK(!cursor->GetValue(oversized_value, direct_read));
                uint8_t key{0}, value{0};
                BOOST_REQUIRE(cursor->GetKey(key, direct_read));
                BOOST_REQUIRE(cursor->GetValue(value, direct_read));
                BOOST_CHECK_EQUAL(key, uint8_t{'j'});
                BOOST_CHECK_EQUAL(value, 42);
            }
        }
    }
}

// Test that we do not obfuscation if there is existing data.
BOOST_AUTO_TEST_CASE(existing_data_no_obfuscate)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_no_obfuscate";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    dbw->Write(key, in);
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Now, set up another wrapper that wants to obfuscate the same directory
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper exists and
    // is readable.
    uint256 res2;
    BOOST_CHECK(odbw.Read(key, res2));
    BOOST_CHECK_EQUAL(res2.ToString(), in.ToString());

    BOOST_CHECK(!odbw.IsEmpty());
    BOOST_CHECK(!dbwrapper_private::GetObfuscation(odbw)); // The key should be an empty string

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    odbw.Write(key, in2);
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

// Ensure that we start obfuscating during a reindex.
BOOST_AUTO_TEST_CASE(existing_data_reindex)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_reindex";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    dbw->Write(key, in);
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Simulate a -reindex by wiping the existing data store
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = true, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper doesn't exist
    uint256 res2;
    BOOST_CHECK(!odbw.Read(key, res2));
    BOOST_CHECK(dbwrapper_private::GetObfuscation(odbw));

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    odbw.Write(key, in2);
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

BOOST_AUTO_TEST_CASE(iterator_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (!(x & 1)) dbw.Write(key, value);
    }

    // Check that creating an iterator creates a snapshot
    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

    for (unsigned int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (x & 1) dbw.Write(key, value);
    }

    for (const int seek_start : {0x00, 0x80}) {
        it->Seek((uint8_t)seek_start);
        for (unsigned int x=seek_start; x<255; ++x) {
            uint8_t key;
            uint32_t value;
            BOOST_CHECK(it->Valid());
            if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                break;
            BOOST_CHECK(it->GetKey(key));
            if (x & 1) {
                BOOST_CHECK_EQUAL(key, x + 1);
                continue;
            }
            BOOST_CHECK(it->GetValue(value));
            BOOST_CHECK_EQUAL(key, x);
            BOOST_CHECK_EQUAL(value, x*x);
            it->Next();
        }
        BOOST_CHECK(!it->Valid());
    }
}

struct StringContentsSerializer {
    // Used to make two serialized objects the same while letting them have different lengths
    // This is a terrible idea
    std::string str;
    StringContentsSerializer() = default;
    explicit StringContentsSerializer(const std::string& inp) : str(inp) {}

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        for (size_t i = 0; i < str.size(); i++) {
            s << uint8_t(str[i]);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        str.clear();
        uint8_t c{0};
        while (!s.empty()) {
            s >> c;
            str.push_back(c);
        }
    }
};

BOOST_AUTO_TEST_CASE(iterator_string_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_string_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            std::string key{ToString(x)};
            for (int z = 0; z < y; ++z)
                key += key;
            uint32_t value = x*x;
            dbw.Write(StringContentsSerializer{key}, value);
        }
    }

    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());
    for (const int seek_start : {0, 5}) {
        it->Seek(StringContentsSerializer{ToString(seek_start)});
        for (unsigned int x = seek_start; x < 10; ++x) {
            for (int y = 0; y < 10; ++y) {
                std::string exp_key{ToString(x)};
                for (int z = 0; z < y; ++z)
                    exp_key += exp_key;
                StringContentsSerializer key;
                uint32_t value;
                BOOST_CHECK(it->Valid());
                if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                    break;
                BOOST_CHECK(it->GetKey(key));
                BOOST_CHECK(it->GetValue(value));
                BOOST_CHECK_EQUAL(key.str, exp_key);
                BOOST_CHECK_EQUAL(value, x*x);
                it->Next();
            }
        }
        BOOST_CHECK(!it->Valid());
    }
}

BOOST_AUTO_TEST_CASE(unicodepath)
{
    // Attempt to create a database with a UTF8 character in the path.
    // On Windows this test will fail if the directory is created using
    // the ANSI CreateDirectoryA call and the code page isn't UTF8.
    // It will succeed if created with CreateDirectoryW.
    fs::path ph = m_args.GetDataDirBase() / "test_runner_₿_🏃_20191128_104644";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20});

    fs::path lockPath = ph / "LOCK";
    BOOST_CHECK(fs::exists(lockPath));
}


BOOST_AUTO_TEST_SUITE_END()
