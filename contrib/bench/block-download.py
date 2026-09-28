#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Localhost block-download pipeline proxy with deterministic regtest blocks.

Adds a fixed response delay per getdata request without blocking the networking
thread. Measures scheduling/validation/disk ingestion, NOT mainnet Yespower,
PRESYNC, Internet bandwidth, late-chain scripts or full IBD completion time.
"""
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'test/functional'))
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, MSG_BLOCK, MSG_TYPE_MASK, msg_block, msg_headers
from test_framework.p2p import NetworkThread, P2PDataStore, p2p_lock
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def process_cpu(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


class DelayedPeer(P2PDataStore):
    def __init__(self, blocks, delay):
        super().__init__()
        self.block_store = blocks
        self.delay = delay
        self.pending = 0
        self.peak_pending = 0
        self.requests = 0
        self.bytes_sent = 0
        self.batch_sizes = []

    def on_getheaders(self, message):
        pass  # The harness announces the complete deterministic header chain.

    def on_getdata(self, message):
        self.batch_sizes.append(sum(inv.type & MSG_TYPE_MASK == MSG_BLOCK for inv in message.inv))
        for inv in message.inv:
            if inv.type & MSG_TYPE_MASK != MSG_BLOCK:
                continue
            self.requests += 1
            self.pending += 1
            self.peak_pending = max(self.peak_pending, self.pending)
            block = self.block_store[inv.hash]
            def deliver(block=block):
                with p2p_lock:
                    self.pending -= 1
                    if self.is_connected:
                        self.bytes_sent += len(block.serialize())
                        self.send_without_ping(msg_block(block))
            NetworkThread.network_event_loop.call_later(self.delay, deliver)


class BlockDownloadBenchmark(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument('--blocks', type=int, default=4096)
        parser.add_argument('--peers', type=int, default=4)
        parser.add_argument('--latency-ms', type=float, default=50)
        parser.add_argument('--expected-inflight', type=int, default=16)
        parser.add_argument('--node-arg', action='append', default=[])
        parser.add_argument('--near-tip', action='store_true', help='exercise transition out of IBD')
        parser.add_argument('--padding-bytes', type=int, default=0, help='valid unspendable output for large-block stress')
        parser.add_argument('--result', type=Path, required=True)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False
        self.extra_args = [['-conf=bitcoin.conf', '-disablewallet=1', '-assumevalid=0',
                            '-v2transport=0', '-dbcache=256', '-debug=0',
                            # Match mainnet's default diagnostic policy; the
                            # separate stall/regression tests keep index audits.
                            '-checkblockindex=0'] + self.options.node_arg]

    def run_test(self):
        opt = self.options
        assert 1 <= opt.blocks <= 20000 and 1 <= opt.peers <= 8
        assert 0 <= opt.latency_ms <= 1000 and opt.expected_inflight >= 1
        assert 0 <= opt.padding_bytes <= 900000
        assert not opt.result.exists()
        node = self.nodes[0]
        assert_equal(node.getpeerinfo(), [])
        tip = int(node.getbestblockhash(), 16)
        stamp = node.getblock(node.getbestblockhash())['time']
        if opt.near_tip:
            stamp = int(time.time()) - opt.blocks - 10
        blocks = []
        padding = CScript([OP_RETURN, bytes(opt.padding_bytes)]) if opt.padding_bytes else None
        for height in range(1, opt.blocks + 1):
            block = create_block(tip, create_coinbase(height, extra_output_script=padding), stamp + height)
            block.solve()
            tip = block.hash_int
            blocks.append(block)
        store = {block.hash_int: block for block in blocks}
        peers = [node.add_outbound_p2p_connection(DelayedPeer(store, opt.latency_ms / 1000),
                 p2p_idx=i, connection_type='outbound-full-relay') for i in range(opt.peers)]
        cpu_start = process_cpu(node.process.pid)
        python_start = time.process_time()
        start = time.monotonic()
        for offset in range(0, len(blocks), 2000):
            message = msg_headers([CBlockHeader(block) for block in blocks[offset:offset + 2000]])
            for peer in peers:
                peer.send_without_ping(message)
        peak_inflight = 0
        while node.getblockcount() < opt.blocks:
            info = node.getpeerinfo()
            assert_equal(len(info), opt.peers)
            peak_inflight = max(peak_inflight, max(len(peer['inflight']) for peer in info))
            assert peak_inflight <= opt.expected_inflight
            assert time.monotonic() - start < 600
            time.sleep(0.02)
        seconds = time.monotonic() - start
        node_cpu = process_cpu(node.process.pid) - cpu_start
        python_cpu = time.process_time() - python_start
        assert_equal(node.getbestblockhash(), blocks[-1].hash_hex)
        assert node.verifychain(4, opt.blocks)
        utxo = node.gettxoutsetinfo()
        with p2p_lock:
            assert all(peer.pending == 0 and peer.peak_pending <= opt.expected_inflight for peer in peers)
            counts = [{'requests': p.requests, 'peak_pending': p.peak_pending, 'bytes_sent': p.bytes_sent} for p in peers]
            if opt.near_tip:
                assert all(size <= 16 for peer in peers for size in peer.batch_sizes[1:])
        assert_equal(node.getblockchaininfo()['initialblockdownload'], not opt.near_tip)
        result = {'proxy': 'localhost_regtest_block_download', 'blocks': opt.blocks,
                  'peers': opt.peers, 'response_delay_ms': opt.latency_ms,
                  'seconds': seconds, 'blocks_per_second': opt.blocks / seconds,
                  'node_cpu_seconds': node_cpu, 'harness_cpu_seconds': python_cpu,
                  'peak_inflight_per_peer': peak_inflight, 'peer_counters': counts,
                  'tip': node.getbestblockhash(), 'utxo': utxo['hash_serialized_3'],
                  'verifychain': True, 'binary_sha256': hashlib.sha256(Path(node.process.args[0]).read_bytes()).hexdigest(),
                  'near_tip': opt.near_tip,
                  'padding_bytes': opt.padding_bytes,
                  'node_args': self.extra_args[0]}
        with opt.result.open('x') as output:
            output.write(json.dumps(result, indent=2) + '\n')
        self.log.info(json.dumps(result))


if __name__ == '__main__':
    BlockDownloadBenchmark(__file__).main()
