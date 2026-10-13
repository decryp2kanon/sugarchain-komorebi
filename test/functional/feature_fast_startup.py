#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Exercise startup mode boundaries without a large historical startup benchmark.

Real Yespower eligibility and failure cases are covered by header_pow_tests.
Here regtest covers mode switching, progress, chainstate and file-presence checks.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.messages import CBlock, CBlockHeader, msg_headers
from test_framework.p2p import P2PInterface
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
            # Algorithms are shared; the flag selects only historical disk trust
            # (and the separately retained DB cache policy).
            messages = ["Counting block index entries...", "202 / 202 (100%)",
                        "Prepared block index during loading: 202 entries"]
            forbidden = ["Preparing block headers...", "Sorting block headers..."]
            with node.assert_debug_log(messages, unexpected_msgs=forbidden):
                self.start_node(0, args)
            actual = node.getblockchaininfo()
            assert_equal(actual["historicalpow"]["state"], "disabled")  # regtest uses SHA256d
            assert_equal(actual["historicalpow"]["historical_trust"], False)
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

        for workers in [0, 9, -1]:
            node.assert_start_raises_init_error(
                [f"-background-pow-workers={workers}"], "-background-pow-workers must be between 1 and 8", match=ErrorMatch.PARTIAL_REGEX)

        for pause in [-1, 1001]:
            node.assert_start_raises_init_error(
                [f"-background-pow-pause={pause}"], "-background-pow-pause must be between 0 and 1000", match=ErrorMatch.PARTIAL_REGEX)

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

        # Headers-only persistence must not wait for the first full block.
        # Exercise the production threshold, then crash ONLY this test's node:
        # the completed atomic prefix survives, the next dirty header does not,
        # and the active chain/UTXO state remains unchanged. Disable the O(N)
        # debug index scan for this large P2P fixture, not header validation.
        self.restart_node(0, ["-fast-startup=0", "-checkblockindex=0"])
        before = node.getblockchaininfo()
        tip = node.getblockheader(before["bestblockhash"])
        threshold = 262_144
        node.setmocktime(tip["time"] + threshold + 100)
        peer = node.add_p2p_connection(P2PInterface())
        previous = int(before["bestblockhash"], 16)
        batch = []
        for i in range(threshold + 1):
            header = CBlock()
            header.hashPrevBlock = previous
            header.nTime = tip["time"] + i + 1
            header.nBits = int(tip["bits"], 16)
            header.solve()
            previous = header.hash_int
            batch.append(CBlockHeader(header))
            if i == threshold - 1:
                persisted_hash = header.hash_hex
            if len(batch) == 2000 or i in (threshold - 1, threshold):
                peer.send_and_ping(msg_headers(batch))
                batch = []
        assert_equal(node.getblockchaininfo()["headers"], before["blocks"] + threshold + 1)
        assert_equal(node.getbestblockhash(), before["bestblockhash"])
        node.disconnect_p2ps()
        node.kill_process()
        self.start_node(0, ["-fast-startup=0", "-checkblockindex=0"])
        after = node.getblockchaininfo()
        assert_equal(after["headers"], before["blocks"] + threshold)
        for key in ["blocks", "bestblockhash", "chainwork"]:
            assert_equal(after[key], before[key])
        assert_equal(node.getblockheader(persisted_hash)["height"], after["headers"])
        assert node.verifychain(3, 20)


if __name__ == '__main__':
    FastStartupTest(__file__).main()
