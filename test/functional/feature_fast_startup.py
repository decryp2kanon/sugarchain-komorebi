#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Exercise startup mode boundaries without a large historical startup benchmark.

Real Yespower eligibility and failure cases are covered by header_pow_tests.
Here regtest covers mode switching, progress, chainstate and file-presence checks.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class FastStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 201)
        expected = node.getblockchaininfo()
        self.stop_node(0)

        for args, fast in [([], False), (["-fast-startup=0"], False),
                           (["-fast-startup=1"], True),
                           (["-fast-startup=1", "-reindex-chainstate=1"], True),
                           (["-fast-startup=0", "-reindex-chainstate=1"], False),
                           (["-fast-startup=0"], False)]:
            self.log.info(f"Startup mode: {args}")
            if fast:
                messages = ["Counting block index entries...", "202 / 202 (100%)",
                            "Prepared block index during loading: 202 entries"]
                forbidden = ["Preparing block headers...", "Sorting block headers..."]
            else:
                messages = ["Loading block index: 202", "Preparing block index...",
                            "Preparing block headers...", "Sorting block headers..."]
                forbidden = ["Counting block index entries...", "Counted 202 block index entries",
                             "Prepared block index during loading:"]
            with node.assert_debug_log(messages, unexpected_msgs=forbidden):
                self.start_node(0, args)
            actual = node.getblockchaininfo()
            for key in ["blocks", "headers", "bestblockhash", "chainwork"]:
                assert_equal(actual[key], expected[key])
            assert node.verifychain(3, 6)
            self.stop_node(0)

        for invalid in ["2", "-1", "true"]:
            node.assert_start_raises_init_error(
                [f"-fast-startup={invalid}"], "-fast-startup must be 0 or 1", match=ErrorMatch.PARTIAL_REGEX)
        node.assert_start_raises_init_error(
            ["-fast-startup=1", "-reindex=1"],
            "-fast-startup=1 cannot be used with -reindex", match=ErrorMatch.PARTIAL_REGEX)

        # Rename only this test's private file and restore it even on failure.
        block_file = node.blocks_path / "blk00000.dat"
        missing_file = block_file.with_suffix(".dat.missing")
        block_file.rename(missing_file)
        try:
            for mode in ["0", "1"]:
                node.assert_start_raises_init_error(
                    [f"-fast-startup={mode}"], "Error loading block database", match=ErrorMatch.PARTIAL_REGEX)
        finally:
            missing_file.rename(block_file)
        self.start_node(0, ["-fast-startup=1"])
        assert_equal(node.getbestblockhash(), expected["bestblockhash"])
        # New blocks written with the fast-mode DB policy survive mode changes.
        self.generate(node, 20)
        updated = node.getblockchaininfo()
        for mode in ["0", "1"]:
            self.stop_node(0)
            self.start_node(0, [f"-fast-startup={mode}"])
            actual = node.getblockchaininfo()
            for key in ["blocks", "headers", "bestblockhash", "chainwork"]:
                assert_equal(actual[key], updated[key])
            assert node.verifychain(3, 20)


if __name__ == '__main__':
    FastStartupTest(__file__).main()
