#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Two localhost peers replay the same real mainnet headers through PRESYNC.

No external peers, blocks or minimum-chainwork overrides. Measures overlap
after a deliberately delayed second peer, not unique cold PoW or full IBD.
"""
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'test/functional'))
from test_framework.messages import CBlockHeader, MAGIC_BYTES, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def process_cpu(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


class Peer(P2PInterface):
    def on_getheaders(self, message):
        pass  # The harness supplies a fixed, complete fixture in wire batches.


class PresyncPeerOverlap(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument('--headers', type=Path, required=True)
        parser.add_argument('--count', type=int, default=40000)
        parser.add_argument('--cache-mib', type=int, default=1)
        parser.add_argument('--profile-wrapper', action='store_true',
                            help='expect the isolated yespower-call-profile link wrapper')
        parser.add_argument('--result', type=Path, required=True)

    def set_test_params(self):
        if not 2000 <= self.options.count <= 1_000_000 or self.options.count % 2000:
            raise ValueError('count must be a multiple of 2000 in 2000..1000000')
        if not 1 <= self.options.cache_mib <= 2048:
            raise ValueError('cache must be 1..2048 MiB')
        self.chain = ''
        MAGIC_BYTES[self.chain] = bytes.fromhex('9feb4b9d')
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False
        self.extra_args = [['-conf=bitcoin.conf', '-disablewallet=1', '-assumevalid=0',
                            '-connect=0', '-dnsseed=0', '-fixedseeds=0', '-discover=0',
                            '-listenonion=0', '-v2transport=0', '-parpow=8',
                            f'-maxpowcache={self.options.cache_mib}']]

    def run_test(self):
        raw = self.options.headers.read_bytes()
        if len(raw) < self.options.count * 80:
            raise ValueError('truncated header fixture')
        headers = []
        for offset in range(0, self.options.count * 80, 80):
            header = CBlockHeader()
            header.deserialize(BytesIO(raw[offset:offset + 80]))
            headers.append(header)
        node = self.nodes[0]
        assert_equal(node.getpeerinfo(), [])
        # addconnection is regtest-only. Use two inbound loopback connections
        # on real mainnet parameters; this measures PRESYNC, not outbound policy.
        peers = [node.add_p2p_connection(Peer(), supports_v2_p2p=False) for _ in range(2)]
        ids = sorted(peer['id'] for peer in node.getpeerinfo())
        assert_equal(len(ids), 2)
        binary = Path(f'/proc/{node.process.pid}/exe').resolve(strict=True)
        binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
        result = {'network': 'localhost only', 'minimum_chainwork': 'unchanged',
                  'fixture_sha256': hashlib.sha256(raw).hexdigest(),
                  'headers': len(headers), 'workers': 8, 'cache_mib': self.options.cache_mib,
                  'binary': str(binary), 'binary_sha256': binary_hash, 'passes': []}
        for i, peer in enumerate(peers):
            cpu, started = process_cpu(node.process.pid), time.monotonic()
            for offset in range(0, len(headers), 2000):
                peer.send_and_ping(msg_headers(headers[offset:offset + 2000]), timeout=240)
                self.wait_until(lambda: any(p['id'] == ids[i] and
                    p.get('presynced_headers') == offset + 2000 for p in node.getpeerinfo()))
            row = {'peer': i, 'seconds': time.monotonic() - started,
                   'cpu_seconds': process_cpu(node.process.pid) - cpu}
            result['passes'].append(row)
            assert_equal(node.getblockchaininfo()['headers'], 0)
            assert_equal(node.getblockcount(), 0)
            assert_equal(len(node.getpeerinfo()), 2)
            self.log.info(json.dumps(row))
        stderr = Path(node.stderr.name)
        expected = re.compile(r'^YESPOWER_PROFILE \{[^\n]+\}$') if self.options.profile_wrapper else ''
        self.stop_node(0, expected_stderr=expected)
        assert_equal(hashlib.sha256(binary.read_bytes()).hexdigest(), binary_hash)
        if self.options.profile_wrapper:
            result['proof_profile'] = json.loads(stderr.read_text().strip().removeprefix('YESPOWER_PROFILE '))
            assert_equal(result['proof_profile']['failed'], 0)
        self.options.result.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    PresyncPeerOverlap(__file__).main()
