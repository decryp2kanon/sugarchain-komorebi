#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Measure first header proofs then RPC block validation in one fresh process.

Uses the functional framework's isolated mainnet node and localhost peer only.
This is NOT network IBD or a block-download scheduler benchmark. No minimum
chainwork override is made; valid blocks are supplied explicitly by submitblock.
Startup, verifychain and restart are outside the two measured phases.
"""
import hashlib
from io import BytesIO
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "test/functional"))

from test_framework.messages import CBlockHeader, MAGIC_BYTES, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class WarmBlockImport(BitcoinTestFramework):
    def set_test_params(self):
        self.chain = ""
        MAGIC_BYTES[self.chain] = bytes.fromhex("9feb4b9d")
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.rpc_timeout = 600  # Uncached baseline verifychain can take minutes.
        self.supports_cli = False
        self.extra_args = [["-conf=bitcoin.conf", "-disablewallet=1", "-assumevalid=0",
                            "-v2transport=0", "-dbcache=1024"]]

    def add_options(self, parser):
        parser.add_argument("--blocks", type=Path, required=True, help="mainnet blk-format file, starting at height 1")
        parser.add_argument("--expected-tip", required=True)
        parser.add_argument("--expected-utxo", required=True, help="hash_serialized_3 of the expected final UTXO set")
        parser.add_argument("--workers", type=int, default=1, choices=range(1, 9))

    def setup_nodes(self):
        # Omit the new argument in baseline serial runs, so pre-optimization
        # binaries can participate without accepting an unknown option.
        if self.options.workers > 1:
            self.extra_args[0].append(f"-parpow={self.options.workers}")
        super().setup_nodes()

    def run_test(self):
        try:
            self.measure()
        finally:
            try:
                self.stop_nodes()
            except Exception as error:
                self.log.warning("RPC shutdown failed; terminate only owned benchmark children: %s", error)
                for node in self.nodes:
                    if node.running and node.process is not None:
                        if node.process.poll() is None:
                            node.process.terminate()
                        node.wait_until_stopped(timeout=600)

    def measure(self):
        node = self.nodes[0]
        assert_equal(node.getblockchaininfo()["chain"], "main")
        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getpeerinfo(), [])
        data = self.options.blocks.read_bytes()
        stream = BytesIO(data)
        blocks, headers = [], []
        while prefix := stream.read(8):
            assert_equal(len(prefix), 8)
            assert_equal(prefix[:4], MAGIC_BYTES[self.chain])
            size = struct.unpack("<I", prefix[4:])[0]
            assert 80 < size <= 4_000_000
            block = stream.read(size)
            assert_equal(len(block), size)
            header = CBlockHeader()
            header.deserialize(BytesIO(block[:80]))
            blocks.append(block.hex())
            headers.append(header)
        assert headers and len(headers) % 2000 == 0, "PRESYNC fixture must contain full 2000-header messages"
        report = {"external_network": False, "assumevalid": "0", "workers": self.options.workers,
                  "blocks": len(blocks), "input_sha256": hashlib.sha256(data).hexdigest(),
                  "binary_sha256": hashlib.sha256(Path(node.process.args[0]).read_bytes()).hexdigest()}
        output = Path(self.options.tmpdir) / "benchmark.json"

        peer = node.add_p2p_connection(P2PInterface(), supports_v2_p2p=False)
        started = time.monotonic()
        for offset in range(0, len(headers), 2000):
            peer.send_and_ping(msg_headers(headers[offset:offset + 2000]))
        report["header_seconds"] = time.monotonic() - started
        # The fixture must remain below the real minimum chainwork. This check
        # prevents presenting bypassed/finished presync as a valid benchmark.
        assert_equal(node.getblockchaininfo()["headers"], 0)
        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getpeerinfo()[0]["presynced_headers"], len(headers))
        output.write_text(json.dumps(report, indent=2) + "\n")

        started = time.monotonic()
        for block in blocks:
            assert_equal(node.submitblock(block), None)
        report["block_seconds"] = time.monotonic() - started
        report["rpc_blocks_per_second"] = len(blocks) / report["block_seconds"]
        assert_equal(node.getblockcount(), len(blocks))
        assert_equal(node.getbestblockhash(), self.options.expected_tip)
        assert_equal(node.verifychain(4, len(blocks)), True)
        utxo = node.gettxoutsetinfo()
        assert_equal(utxo["hash_serialized_3"], self.options.expected_utxo)
        report["utxo"] = utxo
        output.write_text(json.dumps(report, indent=2, default=str) + "\n")

        started = time.monotonic()
        self.stop_node(0)
        report["shutdown_seconds"] = time.monotonic() - started
        self.start_node(0)
        assert_equal(node.getpeerinfo(), [])
        assert_equal(node.getblockcount(), len(blocks))
        assert_equal(node.getbestblockhash(), self.options.expected_tip)
        assert_equal(node.verifychain(4, len(blocks)), True)
        assert_equal(node.gettxoutsetinfo()["hash_serialized_3"], self.options.expected_utxo)
        report["restart_verified"] = True
        output.write_text(json.dumps(report, indent=2, default=str) + "\n")
        self.log.info("Measured phases (not full IBD): %s", json.dumps(report, default=str))


if __name__ == "__main__":
    WarmBlockImport(__file__).main()
