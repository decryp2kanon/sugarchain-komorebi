#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Real Sugarchain headers through the parallel P2P PoW/PRESYNC path, offline.

Only a localhost test peer connects. Mainnet minimum chainwork is unchanged;
6000 headers must remain in PRESYNC, not become an accepted block-index chain.
"""
from io import BytesIO
from pathlib import Path

from test_framework.messages import CBlock, CBlockHeader, MAGIC_BYTES, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class SugarchainHeaderPoWTest(BitcoinTestFramework):
    def set_test_params(self):
        # Empty chain subdirectory selects mainnet without changing shared
        # Bitcoin/regtest fixtures or production defaults.
        self.chain = ""
        MAGIC_BYTES[self.chain] = bytes.fromhex("9feb4b9d")
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False
        self.extra_args = [["-conf=bitcoin.conf", "-disablewallet=1", "-parpow=8",
                            "-assumevalid=0", "-v2transport=0", "-maxpowcache=1"]]

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getblockchaininfo()["chain"], "main")
        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getpeerinfo(), [])
        fixture = Path(__file__).resolve().parents[2] / "src/test/data/sugarchain_headers.raw"
        raw = fixture.read_bytes()
        assert_equal(len(raw), 6000 * 80)
        headers = []
        for offset in range(0, len(raw), 80):
            header = CBlockHeader()
            header.deserialize(BytesIO(raw[offset:offset + 80]))
            headers.append(header)

        peer = node.add_p2p_connection(P2PInterface(), supports_v2_p2p=False)
        for offset in range(0, len(headers), 2000):
            peer.send_and_ping(msg_headers(headers[offset:offset + 2000]))
            self.wait_until(lambda: node.getpeerinfo()[0]["presynced_headers"] == offset + 2000)
        assert_equal(node.getblockchaininfo()["headers"], 0)
        assert_equal(node.getblockcount(), 0)

        # This exact nonce mutation was also checked with the uncached primitive;
        # keep valid nBits so this exercises genuine Yespower rejection.
        bad = CBlockHeader(headers[0])
        bad.nNonce ^= 1
        with node.assert_debug_log(["header with invalid proof of work"]):
            peer.send_without_ping(msg_headers([bad]))
            peer.wait_for_disconnect()
        self.wait_until(lambda: not node.getpeerinfo())
        assert_equal(node.getblockcount(), 0)

        # The existing 2000-header resource bound must still reject before any
        # parallel batch is admitted, even though all supplied headers are valid.
        peer = node.add_p2p_connection(P2PInterface(), supports_v2_p2p=False)
        with node.assert_debug_log(["headers message size = 2001"]):
            peer.send_without_ping(msg_headers(headers[:2001]))
            peer.wait_for_disconnect()
        self.wait_until(lambda: not node.getpeerinfo())

        # Destroy a node-owned worker pool, restart, and verify that fresh
        # presync state is usable. No persisted flag substitutes for proof work.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-maxpowcache=2"])
        peer = node.add_p2p_connection(P2PInterface(), supports_v2_p2p=False)
        peer.send_and_ping(msg_headers(headers[:2000]))
        self.wait_until(lambda: node.getpeerinfo()[0]["presynced_headers"] == 2000)
        assert_equal(node.getblockchaininfo()["headers"], 0)
        assert_equal(node.getblockcount(), 0)

        # A cached valid header proves nothing about the supplied block body.
        # Mutate only the coinbase output, leaving the header/PoW unchanged.
        raw_block = bytes.fromhex((Path(__file__).resolve().parent / "data/sugarchain_block_1.hex").read_text())
        block = CBlock()
        block.deserialize(BytesIO(raw_block))
        assert_equal(CBlockHeader(block).serialize(), headers[0].serialize())
        block.vtx[0].vout[0].nValue -= 1
        assert_equal(node.submitblock(block.serialize().hex()), "bad-txnmrklroot")
        assert_equal(node.getblockcount(), 0)
        # Rejecting the mutated body must not poison the genuine block either.
        assert_equal(node.submitblock(raw_block.hex()), None)
        assert_equal(node.getblockcount(), 1)
        assert_equal(node.getbestblockhash(), "ce8a0df339f2edceb99c5325c95b2b0ae752e29de1193f6113549f0e1cae7c91")

        # Invalid budgets must fail startup rather than silently requesting an
        # unbounded allocation. Only these temporary test nodes are stopped.
        self.stop_node(0)
        for budget in ("0", "-1", "2049", "invalid"):
            node.assert_start_raises_init_error(
                extra_args=self.extra_args[0] + [f"-maxpowcache={budget}"],
                expected_msg="-maxpowcache must be between 1 and 2048 MiB",
                match=ErrorMatch.PARTIAL_REGEX,
            )


if __name__ == "__main__":
    SugarchainHeaderPoWTest(__file__).main()
