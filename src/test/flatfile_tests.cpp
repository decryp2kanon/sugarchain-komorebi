// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <common/args.h>
#include <flatfile.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <span>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

BOOST_FIXTURE_TEST_SUITE(flatfile_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(flatfile_filename)
{
    const auto data_dir = m_args.GetDataDirBase();

    FlatFilePos pos(456, 789);

    FlatFileSeq seq1(data_dir, "a", 16 * 1024);
    BOOST_CHECK_EQUAL(seq1.FileName(pos), data_dir / "a00456.dat");

    FlatFileSeq seq2(data_dir / "a", "b", 16 * 1024);
    BOOST_CHECK_EQUAL(seq2.FileName(pos), data_dir / "a" / "b00456.dat");

    // Check default constructor IsNull
    assert(FlatFilePos{}.IsNull());
}

BOOST_AUTO_TEST_CASE(flatfile_open)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 16 * 1024);

    std::string line1("A purely peer-to-peer version of electronic cash would allow online "
                      "payments to be sent directly from one party to another without going "
                      "through a financial institution.");
    std::string line2("Digital signatures provide part of the solution, but the main benefits are "
                      "lost if a trusted third party is still required to prevent double-spending.");

    uint64_t pos1{0};
    uint64_t pos2{pos1 + GetSerializeSize(line1)};

    // Write first line to file.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos1))};
        file << LIMITED_STRING(line1, 256);
        BOOST_REQUIRE_EQUAL(file.fclose(), 0);
    }

    // Attempt to append to file opened in read-only mode.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos2), true)};
        BOOST_CHECK_THROW(file << LIMITED_STRING(line2, 256), std::ios_base::failure);
    }

    // Append second line to file.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos2))};
        file << LIMITED_STRING(line2, 256);
        BOOST_REQUIRE_EQUAL(file.fclose(), 0);
    }

    // Read text from file in read-only mode.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(0, pos1), true)};

        file >> LIMITED_STRING(text, 256);
        BOOST_CHECK_EQUAL(text, line1);

        file >> LIMITED_STRING(text, 256);
        BOOST_CHECK_EQUAL(text, line2);
    }

    // Read text from file with position offset.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(0, pos2))};

        file >> LIMITED_STRING(text, 256);
        BOOST_CHECK_EQUAL(text, line2);
        BOOST_REQUIRE_EQUAL(file.fclose(), 0);
    }

    // Ensure another file in the sequence has no data.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(1, pos2))};
        BOOST_CHECK_THROW(file >> LIMITED_STRING(text, 256), std::ios_base::failure);
        BOOST_REQUIRE_EQUAL(file.fclose(), 0);
    }
}

BOOST_AUTO_TEST_CASE(flatfile_open_directory_lifecycle)
{
    const auto directory{m_args.GetDataDirBase() / "nested" / "blocks"};
    FlatFileSeq seq{directory, "blk", 4096};
    const FlatFilePos pos{0, 0};
    BOOST_CHECK(!seq.Open(FlatFilePos{}));
    BOOST_CHECK(!fs::exists(directory));
    // Preserve the existing read-only failure/parent-creation behavior.
    BOOST_CHECK(!seq.Open(pos, true));
    BOOST_CHECK(fs::is_directory(directory));
    BOOST_CHECK(!fs::exists(seq.FileName(pos)));
    for (int pass{0}; pass < 2; ++pass) {
        {
            AutoFile file{seq.Open(pos)};
            BOOST_REQUIRE(!file.IsNull());
            file << uint32_t{123456};
            BOOST_REQUIRE_EQUAL(file.fclose(), 0);
        }
        // Opening an existing file must not truncate it.
        {
            AutoFile file{seq.Open(pos)};
            uint32_t value{};
            file >> value;
            BOOST_CHECK_EQUAL(value, 123456U);
            BOOST_REQUIRE_EQUAL(file.fclose(), 0);
        }
        BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(pos)), sizeof(uint32_t));
        BOOST_CHECK(fs::remove(seq.FileName(pos)));
        BOOST_CHECK(fs::remove(directory));
        // The next open must recreate a removed parent; no cached existence bit.
    }
}

BOOST_AUTO_TEST_CASE(flatfile_offset_stream_position_and_contents)
{
    FlatFileSeq seq{m_args.GetDataDirBase(), "offset", 4096};
    std::string expected(16384, '\0');
    for (size_t i{0}; i < expected.size(); ++i) expected[i] = char(i % 251);
    {
        AutoFile file{seq.Open({0, 0})};
        file.write(std::as_bytes(std::span{expected}));
        BOOST_REQUIRE_EQUAL(file.fclose(), 0);
    }
    // Include unaligned/page-boundary offsets, existing data and a sparse gap.
    for (const unsigned int offset : {0U, 1U, 255U, 4095U, 4096U, 4097U, 8192U, 16640U}) {
        for (const bool read_only : {false, true}) {
            FILE* file{seq.Open({0, offset}, read_only)};
            BOOST_REQUIRE(file);
            BOOST_CHECK_EQUAL(ftell(file), offset);
            const int value{fgetc(file)};
            if (offset < expected.size()) {
                BOOST_CHECK_EQUAL(value, static_cast<unsigned char>(expected[offset]));
                BOOST_CHECK_EQUAL(ftell(file), offset + 1);
            } else {
                BOOST_CHECK_EQUAL(value, EOF);
                BOOST_CHECK(feof(file));
                BOOST_CHECK_EQUAL(ftell(file), offset);
            }
            BOOST_REQUIRE_EQUAL(fclose(file), 0);
        }
        const std::array<unsigned char, 5> replacement{1, 2, 3, 4, 5};
        {
            FILE* file{seq.Open({0, offset})};
            BOOST_REQUIRE(file);
            BOOST_CHECK_EQUAL(ftell(file), offset);
            BOOST_REQUIRE_EQUAL(fwrite(replacement.data(), 1, replacement.size(), file), replacement.size());
            BOOST_CHECK_EQUAL(ftell(file), offset + replacement.size());
            // An explicit seek switches an update stream from writing to reading.
            BOOST_REQUIRE_EQUAL(fseek(file, offset, SEEK_SET), 0);
            for (const auto value : replacement) BOOST_CHECK_EQUAL(fgetc(file), value);
            BOOST_REQUIRE_EQUAL(fclose(file), 0);
        }
        if (expected.size() < offset + replacement.size()) expected.resize(offset + replacement.size(), '\0');
        std::copy(replacement.begin(), replacement.end(), expected.begin() + offset);
        std::ifstream input{seq.FileName({0, 0}).std_path(), std::ios::binary};
        const std::string actual{std::istreambuf_iterator<char>{input}, {}};
        BOOST_CHECK(actual == expected); // No truncation, shifted write or changed gap.
        BOOST_CHECK_EQUAL(fs::file_size(seq.FileName({0, 0})), expected.size());
    }
    BOOST_CHECK(seq.Flush({0, static_cast<unsigned int>(expected.size())}));
}

#ifdef __linux__
BOOST_AUTO_TEST_CASE(flatfile_failed_seek_closes_descriptor)
{
    FlatFileSeq seq{m_args.GetDataDirBase(), "fifo", 4096};
    // Linux permits opening a FIFO read/write without a peer; seeking fails.
    BOOST_REQUIRE_EQUAL(mkfifo(seq.FileName({0, 0}).c_str(), 0600), 0);
    const int available{open("/dev/null", O_RDONLY)};
    BOOST_REQUIRE_GE(available, 0);
    BOOST_REQUIRE_EQUAL(close(available), 0);
    for (int attempt{0}; attempt < 8; ++attempt) {
        BOOST_CHECK(!seq.Open({0, 1}));
        const int probe{open("/dev/null", O_RDONLY)};
        BOOST_REQUIRE_GE(probe, 0);
        BOOST_CHECK_EQUAL(probe, available); // Failed seek must not leak the fd.
        BOOST_REQUIRE_EQUAL(close(probe), 0);
    }
}
#endif

BOOST_AUTO_TEST_CASE(flatfile_open_non_directory_parent)
{
    const auto blocker{m_args.GetDataDirBase() / "not-a-directory"};
    { std::ofstream file{blocker.std_path()}; file << "preserve"; }
    FlatFileSeq seq{blocker / "blocks", "blk", 4096};
    BOOST_CHECK_THROW(seq.Open({0, 0}), fs::filesystem_error);
    BOOST_CHECK_THROW(seq.Open({0, 0}, true), fs::filesystem_error);
    BOOST_CHECK_EQUAL(fs::file_size(blocker), 8U);
}

BOOST_AUTO_TEST_CASE(flatfile_allocate)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 100);

    bool out_of_space;

    BOOST_CHECK_EQUAL(seq.Allocate(FlatFilePos(0, 0), 1, out_of_space), 100U);
    BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(FlatFilePos(0, 0))), 100U);
    BOOST_CHECK(!out_of_space);

    BOOST_CHECK_EQUAL(seq.Allocate(FlatFilePos(0, 99), 1, out_of_space), 0U);
    BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(FlatFilePos(0, 99))), 100U);
    BOOST_CHECK(!out_of_space);

    BOOST_CHECK_EQUAL(seq.Allocate(FlatFilePos(0, 99), 2, out_of_space), 101U);
    BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(FlatFilePos(0, 99))), 200U);
    BOOST_CHECK(!out_of_space);
}

BOOST_AUTO_TEST_CASE(flatfile_flush)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 100);

    bool out_of_space;
    seq.Allocate(FlatFilePos(0, 0), 1, out_of_space);

    // Flush without finalize should not truncate file.
    seq.Flush(FlatFilePos(0, 1));
    BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(FlatFilePos(0, 1))), 100U);

    // Flush with finalize should truncate file.
    seq.Flush(FlatFilePos(0, 1), true);
    BOOST_CHECK_EQUAL(fs::file_size(seq.FileName(FlatFilePos(0, 1))), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
