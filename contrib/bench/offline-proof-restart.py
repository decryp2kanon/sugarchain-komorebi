#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Offline proof-evidence lifecycle test using a separately trace-linked daemon.

Import a supplied real fixture, verify every header has a genuine Yespower call,
restart and check that the evidence is recomputed, then reject disk corruption.
No public peers, trust-anchor overrides or performance measurement.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'test/functional'))
from test_framework.messages import MAGIC_BYTES
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

MAGIC = bytes.fromhex('9feb4b9d')
REPORT = re.compile(r'^YESPOWER_TRACE \{[^\n]+\}$')


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def fixture_headers(path):
    headers = []
    with path.open('rb') as stream:
        while prefix := stream.read(8):
            if len(prefix) != 8 or prefix[:4] != MAGIC:
                raise ValueError('Invalid fixture record')
            size = struct.unpack('<I', prefix[4:])[0]
            if not 80 < size <= 4_000_000 or len(headers) >= 10000:
                raise ValueError('Fixture bounds exceeded')
            block = stream.read(size)
            if len(block) != size:
                raise ValueError('Truncated fixture')
            headers.append(block[:80])
    return headers


class OfflineProofRestart(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument('--blocks', type=Path, required=True)
        parser.add_argument('--height', type=int, required=True)
        parser.add_argument('--tip', required=True)
        parser.add_argument('--utxo', required=True)
        parser.add_argument('--result', type=Path, required=True)

    def set_test_params(self):
        self.chain = ''
        MAGIC_BYTES[self.chain] = MAGIC
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.bind_to_localhost_only = False # Listening is entirely disabled below.
        self.supports_cli = False
        self.options.blocks = self.options.blocks.resolve(strict=True)
        if not 1 <= self.options.height <= 10000:
            raise ValueError('height must be 1..10000')
        for value in (self.options.tip, self.options.utxo):
            if not re.fullmatch('[0-9a-f]{64}', value):
                raise ValueError('tip/utxo must be lowercase 256-bit hex')
        if self.options.result.exists():
            raise ValueError('result already exists')
        self.base_args = ['-conf=bitcoin.conf', '-disablewallet=1', '-assumevalid=0',
                          '-networkactive=0', '-connect=0', '-dnsseed=0', '-fixedseeds=0',
                          '-listen=0', '-discover=0', '-listenonion=0', '-blocksxor=0',
                          '-parpow=8', '-checklevel=4', f'-checkblocks={self.options.height}']
        self.extra_args = [self.base_args + [f'-loadblock={self.options.blocks}']]
        self.result = {'kind': 'offline proof lifecycle; NOT a performance benchmark',
                       'network': False, 'minimum_chainwork': 'unchanged', 'passes': []}

    def trace_path(self, stage):
        return self.options.result.with_suffix(f'.{stage}.proofs')

    def trace_env(self, stage):
        path = self.trace_path(stage)
        if path.exists():
            raise ValueError(f'Trace already exists: {path}')
        return {**os.environ, 'YESPOWER_PROOF_TRACE': str(path)}

    def setup_network(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args)
        self.start_node(0, env=self.trace_env('initial'))

    def inspect_trace(self, stage, stderr, expected):
        rows = [json.loads(line.removeprefix('YESPOWER_TRACE ')) for line in stderr.splitlines()
                if line.startswith('YESPOWER_TRACE ')]
        assert_equal(len(rows), 1)
        summary = rows[0]
        assert summary['parameters_ok'] and summary['written'] and not summary['overflow']
        assert_equal(summary['failures'], 0)
        raw = self.trace_path(stage).read_bytes()
        assert_equal(raw[:4], b'YPT1')
        assert_equal((len(raw) - 4) % 88, 0)
        records = {raw[p:p + 80]: struct.unpack('<Q', raw[p + 80:p + 88])[0]
                   for p in range(4, len(raw), 88)}
        assert_equal(len(records), summary['unique'])
        assert_equal(sum(records.values()), summary['calls'])
        assert_equal(set(records), expected)
        # Genesis can be checked by more than one startup assertion. Each real
        # fixture header must receive exactly one genuine proof in EACH process.
        for header in self.headers:
            assert_equal(records[header], 1)
        self.result['passes'].append({'stage': stage, **summary, 'every_fixture_header_proved': True})
        return records

    def check_chainstate(self):
        node = self.nodes[0]
        self.wait_until(lambda: node.getblockcount() == self.options.height, timeout=300)
        assert_equal(node.getpeerinfo(), [])
        assert_equal(node.getnetworkinfo()['networkactive'], False)
        assert_equal(node.getbestblockhash(), self.options.tip)
        assert node.verifychain(4, self.options.height)
        assert_equal(node.gettxoutsetinfo('hash_serialized_3')['hash_serialized_3'], self.options.utxo)

    def run_test(self):
        node = self.nodes[0]
        self.headers = fixture_headers(self.options.blocks)
        assert_equal(len(self.headers), self.options.height)
        # The nonce mutation below is independently covered by the real-header
        # unit regression fixture; don't apply it to an arbitrary first header.
        assert_equal(self.headers[0], (ROOT / 'src/test/data/sugarchain_headers.raw').read_bytes()[:80])
        digest = file_hash(self.options.blocks)
        binary = Path(f'/proc/{node.process.pid}/exe').resolve(strict=True)
        self.result.update({'fixture_sha256': digest, 'binary_sha256': file_hash(binary),
                            'height': self.options.height, 'tip': self.options.tip, 'utxo': self.options.utxo})
        self.check_chainstate()
        genesis = bytes.fromhex(node.getblockheader(node.getblockhash(0), False))
        expected = set(self.headers) | {genesis}
        assert_equal(len(expected), self.options.height + 1)
        stderr = Path(node.stderr.name)
        self.stop_node(0, expected_stderr=REPORT)
        self.inspect_trace('initial', stderr.read_text(), expected)
        self.start_node(0, extra_args=self.base_args, env=self.trace_env('restart'))
        self.check_chainstate()
        stderr = Path(node.stderr.name)
        self.stop_node(0, expected_stderr=REPORT)
        self.inspect_trace('restart', stderr.read_text(), expected)

        block_file = node.blocks_path / 'blk00000.dat'
        with block_file.open('r+b') as stream:
            while True:
                pos = stream.tell()
                prefix = stream.read(8)
                assert_equal(prefix[:4], MAGIC)
                size = struct.unpack('<I', prefix[4:])[0]
                header = stream.read(80)
                if header == self.headers[0]:
                    nonce_pos = pos + 8 + 76
                    original = header[76:80]
                    changed = struct.pack('<I', struct.unpack('<I', original)[0] ^ 1)
                    corrupt_header = header[:76] + changed
                    stream.seek(nonce_pos)
                    stream.write(changed)
                    break
                stream.seek(pos + 8 + size)
        # The test owns this disposable datadir. Preserve the supplied fixture;
        # restore our changed bytes even when the expected rejection test fails.
        try:
            node.assert_start_raises_init_error(extra_args=self.base_args,
                expected_msg='Corrupted block database detected', match=ErrorMatch.PARTIAL_REGEX,
                env=self.trace_env('corrupt'))
            records = self.inspect_trace('corrupt', Path(node.stderr.name).read_text(), expected | {corrupt_header})
            assert_equal(records[corrupt_header], 1)
            self.result['corrupt_disk_header_rejected'] = True
        finally:
            with block_file.open('r+b') as stream:
                stream.seek(nonce_pos)
                stream.write(original)
        assert_equal(file_hash(self.options.blocks), digest)
        self.options.result.write_text(json.dumps(self.result, indent=2) + '\n')
        self.log.info(json.dumps(self.result))


if __name__ == '__main__':
    OfflineProofRestart(__file__).main()
