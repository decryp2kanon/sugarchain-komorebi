#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""A/B/B/A localhost supply proxy; never public-mainnet or full IBD."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--configfile', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--case', choices=['mixed', 'uniform', 'latency'], required=True)
    parser.add_argument('--blocks', type=int, default=4096)
    parser.add_argument('--limit', type=int, default=128)
    parser.add_argument('--latency-ms', type=float, default=50)
    parser.add_argument('--portseed', type=int, default=62)
    args = parser.parse_args()
    if not 1 <= args.blocks <= 20000 or not 16 <= args.limit <= 128 or not 0 <= args.latency_ms <= 1000:
        parser.error('blocks must be 1..20000, limit 16..128, latency 0..1000ms')
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ('baseline', 'candidate')}
    script = Path(__file__).with_name('block-download.py').resolve(strict=True)
    hashes = {str(path): digest(path) for path in (*binaries.values(), script)}
    args.work_dir.mkdir(parents=True, exist_ok=False)
    services = {'mixed': [20, 1, 1, 1], 'uniform': [4] * 4, 'latency': [0] * 4}[args.case]
    report = {'kind': 'localhost regtest supply proxy; NOT full IBD', 'case': args.case,
              'sha256': hashes, 'runs': []}
    expected_chain = None
    for number, variant in enumerate(('baseline', 'candidate', 'candidate', 'baseline')):
        if any(digest(Path(path)) != value for path, value in hashes.items()):
            raise RuntimeError('Executable or harness changed during A/B')
        prefix = args.work_dir / f'{number}-{variant}'
        result_path = prefix.with_suffix('.json').resolve()
        command = [sys.executable, str(script), f'--configfile={args.configfile.resolve()}',
                   f'--blocks={args.blocks}', '--peers=4', f'--latency-ms={args.latency_ms}',
                   f'--node-arg=-maxibdblocksinflight={args.limit}', f'--expected-inflight={args.limit}',
                   f'--result={result_path}', f'--tmpdir={prefix.resolve()}', f'--portseed={args.portseed}',
                   '--nocleanup'] + [f'--peer-service-ms={value}' for value in services]
        with prefix.with_suffix('.log').open('x') as output:
            subprocess.run(command, env={**os.environ, 'BITCOIND': str(binaries[variant])},
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=660)
        result = json.loads(result_path.read_text())
        if (result['binary_sha256'] != hashes[str(binaries[variant])] or not result['verifychain']
                or result['blocks'] != args.blocks or result['peak_inflight_per_peer'] > args.limit
                or not math.isfinite(result['seconds']) or result['seconds'] <= 0):
            raise RuntimeError('Invalid measurement or resource/correctness failure')
        chain = (result['tip'], result['utxo'])
        if expected_chain is not None and chain != expected_chain:
            raise RuntimeError('A/B chainstate mismatch')
        expected_chain = chain
        report['runs'].append({'variant': variant, 'result': result})
        (args.work_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(variant, result['seconds'], flush=True)
    if any(digest(Path(path)) != value for path, value in hashes.items()):
        raise RuntimeError('Executable or harness changed during A/B')
    medians = {name: statistics.median(r['result']['seconds'] for r in report['runs']
                                      if r['variant'] == name) for name in binaries}
    report['median_seconds'] = medians
    report['proxy_speedup'] = medians['baseline'] / medians['candidate']
    (args.work_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(medians), flush=True)


if __name__ == '__main__':
    main()
