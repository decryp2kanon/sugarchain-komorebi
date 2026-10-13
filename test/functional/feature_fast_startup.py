#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Exercise startup mode boundaries without a large historical startup benchmark.

Real Yespower eligibility and failure cases are covered by header_pow_tests.
Here regtest covers mode switching, progress, chainstate and file-presence checks.
It also exercises full-block-only and headers-only persistence through the actual
P2P validation paths in both startup modes: a completed storage boundary must
survive a private node crash, while its next unpersisted suffix must not. This
verifies persistence and chainstate behavior with regtest SHA256d; it does not
benchmark mainnet IBD, exercise genuine Yespower evidence, or simulate a power
failure during a write.
"""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.test_framework import BitcoinTestFramework
from test_framework.messages import CBlock, CBlockHeader, msg_block, msg_headers
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

        # Full blocks enter AcceptBlock directly, without ProcessNewBlockHeaders.
        # Send no header announcements here: otherwise the existing header-count
        # trigger could persist the same entries and hide missing PERIODIC writes
        # during ordinary block activation. Run before the headers-only fixture,
        # whose higher-work branch intentionally has no corresponding block data.
        threshold = 32_768  # BlockManager.MAX_DIRTY_BLOCK_INDEX, as below.
        for mode in ["0", "1"]:
            self.log.info(f"Full-block persistence boundary in startup mode {mode}")
            before = node.getblockchaininfo()
            tip = node.getblockheader(before["bestblockhash"])
            # Restart flushes prior changes. Set mocktime at startup, before the
            # periodic timer is initialized, then keep it fixed throughout the
            # fixture so time-based writes cannot masquerade as the count trigger.
            # All block timestamps increase but remain earlier than mocktime.
            # The coins budget also keeps this small fixture below pressure-driven
            # flushing. Only the expensive debug index scan is disabled.
            args = [f"-fast-startup={mode}", "-checkblockindex=0", "-dbcache=64",
                    f'-mocktime={tip["time"] + threshold + 100}']
            self.restart_node(0, args)
            peer = node.add_p2p_connection(P2PInterface())
            previous = int(tip["hash"], 16)
            for i in range(threshold + 1):
                height = before["blocks"] + i + 1
                block = create_block(previous, create_coinbase(height), tip["time"] + i + 1)
                block.solve()
                previous = block.hash_int
                peer.send_without_ping(msg_block(block))
                # Drain the real P2P processing path in bounded groups, and
                # separately at the completed write and its one-block suffix.
                if (i + 1) % 1000 == 0 or i in (threshold - 1, threshold):
                    peer.sync_with_ping(timeout=120)
                    assert_equal(node.getblockcount(), height)
                if i == threshold - 1:
                    persisted = node.getblockchaininfo()
                    persisted_coinbase = block.vtx[0].txid_hex
                    assert_equal(persisted["bestblockhash"], block.hash_hex)
                    assert_equal(persisted["headers"], height)
                    assert node.gettxout(persisted_coinbase, 0) is not None

            suffix = block
            suffix_coinbase = suffix.vtx[0].txid_hex
            assert_equal(node.getbestblockhash(), suffix.hash_hex)
            assert node.gettxout(suffix_coinbase, 0) is not None
            node.disconnect_p2ps()
            # Kill ONLY this test node after the boundary's synchronous write.
            # A graceful shutdown would persist the suffix and invalidate the
            # test; this is not mid-write or power-loss fault injection.
            node.kill_process()
            self.start_node(0, args)
            after = node.getblockchaininfo()
            for key in ["blocks", "headers", "bestblockhash", "chainwork"]:
                assert_equal(after[key], persisted[key])
            assert node.gettxout(persisted_coinbase, 0) is not None
            assert_equal(node.gettxout(suffix_coinbase, 0), None)
            assert node.verifychain(4, 100)

            # Reaccept the missing block to prove the recovered UTXO/index tip
            # can advance again. The next mode starts with a clean restart and
            # fresh blocks rather than merely submitting duplicate block data.
            peer = node.add_p2p_connection(P2PInterface())
            peer.send_and_ping(msg_block(suffix))
            assert_equal(node.getbestblockhash(), suffix.hash_hex)
            assert node.gettxout(suffix_coinbase, 0) is not None
            node.disconnect_p2ps()

        # Regression: headers-first replay used to accumulate dirty indexes until
        # full-block processing triggered persistence. Exercise the count-triggered
        # FlushStateToDisk path with a flushable active chainstate in BOTH modes.
        # Send one full persistence interval plus one extra header, then kill ONLY
        # this private test node without a graceful shutdown that would save the
        # extra header. After restart, require exactly the completed prefix, with
        # the active block tip, chainwork and verifychain result preserved.
        # This kills the process after the boundary write has completed; it does
        # not inject a failure in the middle of an atomic write or test power loss.
        # Disable the O(N) debug index scan for this large P2P fixture, not header
        # validation, so checking every new header does not make the test O(N^2).
        header_tip = None
        for mode in ["0", "1"]:
            before = node.getblockchaininfo()
            tip = node.getblockheader(header_tip or before["bestblockhash"])
            # Match BlockManager.MAX_DIRTY_BLOCK_INDEX, reduced from 262144 to
            # 32768. Test the real storage boundary rather than a test-only limit.
            # Keep this fixture aligned when changing the production threshold;
            # its benchmark rationale is in doc/ibd-rss-management.md.
            threshold = 32_768
            # Keep the clock valid for the preceding full-block fixture as well
            # as this header chain. Reuse the same startup clock after the crash;
            # reverting to wall time could reject our deliberately advanced tip
            # as a future block before the persistence assertions can run.
            args = [f"-fast-startup={mode}", "-checkblockindex=0",
                    f'-mocktime={tip["time"] + threshold + 100}']
            self.restart_node(0, args)
            peer = node.add_p2p_connection(P2PInterface())
            previous = int(tip["hash"], 16)
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
                # Respect the 2000-header P2P message limit and finish separate
                # messages exactly at the durable boundary and its dirty suffix.
                # The ping ensures the node has handled each preceding message
                # before the test proceeds to the process kill below.
                if len(batch) == 2000 or i in (threshold - 1, threshold):
                    peer.send_and_ping(msg_headers(batch))
                    batch = []
            assert_equal(node.getblockchaininfo()["headers"], tip["height"] + threshold + 1)
            assert_equal(node.getbestblockhash(), before["bestblockhash"])
            node.disconnect_p2ps()
            node.kill_process()
            self.start_node(0, args)
            after = node.getblockchaininfo()
            # The suffix was accepted in memory before the crash, but only the
            # threshold-sized prefix was durable. Also check the surviving tip's
            # identity, not just its height, alongside the unchanged active chain.
            assert_equal(after["headers"], tip["height"] + threshold)
            for key in ["blocks", "bestblockhash", "chainwork"]:
                assert_equal(after[key], before[key])
            assert_equal(node.getblockheader(persisted_hash)["height"], after["headers"])
            assert node.verifychain(3, 20)
            # Extend this durable tip in the next mode with fresh headers.
            # Replaying the same headers would only exercise duplicate handling
            # and could pass without triggering another real persistence boundary.
            header_tip = persisted_hash


if __name__ == '__main__':
    FastStartupTest(__file__).main()
