#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""A/B/B/A indexed-header block-validation component benchmark, never network IBD."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import statistics
import subprocess


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def measurements(output, count, mode, tip, utxo):
    lines = output.splitlines()
    rows = list(csv.DictReader(io.StringIO('\n'.join(lines[:3]))))
    if len(rows) != 2 or [r['phase'] for r in rows] != ['headers', mode]:
        raise ValueError('Missing header/block measurement phases')
    for row in rows:
        if int(row['blocks']) != count or int(row['yespower_calls']) < 0 or int(row['yespower_cpu_ns']) < 0:
            raise ValueError('Invalid counters')
        seconds = float(row['seconds'])
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError('Invalid duration')
    if int(rows[0]['yespower_calls']) < count:
        raise ValueError('Missing genuine first header proofs')
    if f'tip={tip} utxo={utxo}' not in lines or 'verifychain=PASS' not in lines:
        raise ValueError('Missing/mismatched chainstate or verifychain result')
    return {r['phase']: {'seconds': float(r['seconds']), 'yespower_calls': int(r['yespower_calls']),
                         'yespower_cpu_ns': int(r['yespower_cpu_ns'])} for r in rows}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--blocks', type=Path, required=True)
    p.add_argument('--count', type=int, required=True)
    p.add_argument('--tip', required=True)
    p.add_argument('--utxo', required=True)
    p.add_argument('--mode', choices=['cold', 'warm'], default='cold', help='block-stage proof cache reset/retained')
    p.add_argument('--require-candidate-reuse', action='store_true')
    p.add_argument('--baseline-script-workers', type=int, choices=range(16), metavar='0..15',
                   help='script workers, NOT Yespower workers; requires the extended benchmark')
    p.add_argument('--candidate-script-workers', type=int, choices=range(16), metavar='0..15')
    p.add_argument('--dbcache-mib', type=int, help='same DB cache budget for both arms (4..16384)')
    p.add_argument('--timeout', type=int, default=1200)
    p.add_argument('--work-dir', type=Path, required=True, help='must not exist')
    args = p.parse_args()
    if not 1 <= args.count <= 100000 or args.timeout <= 0:
        p.error('count must be 1..100000 and timeout positive')
    if args.dbcache_mib is not None and not 4 <= args.dbcache_mib <= 16384:
        p.error('dbcache-mib must be 4..16384')
    for value in (args.tip, args.utxo):
        if len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
            p.error('tip/utxo must be lowercase 256-bit hex')
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ('baseline', 'candidate')}
    blocks = args.blocks.resolve(strict=True)
    hashes = {str(path): digest(path) for path in (*binaries.values(), blocks)}
    args.work_dir.mkdir(parents=True, exist_ok=False)
    report = {'kind': 'offline indexed block component; NOT full IBD', 'mode': args.mode,
              'sha256': hashes, 'runs': []}
    for number, name in enumerate(('baseline', 'candidate', 'candidate', 'baseline')):
        if any(digest(Path(path)) != value for path, value in hashes.items()):
            raise RuntimeError('Input/executable changed during comparison')
        command = [str(binaries[name]), str(blocks), args.mode]
        workers = getattr(args, f'{name}_script_workers')
        if workers is not None or args.dbcache_mib is not None:
            command.append(str(2 if workers is None else workers))
        if args.dbcache_mib is not None:
            command.append(str(args.dbcache_mib))
        process = subprocess.run(command, capture_output=True,
                                 text=True, timeout=args.timeout)
        (args.work_dir / f'{number}-{name}.stdout').write_text(process.stdout)
        (args.work_dir / f'{number}-{name}.stderr').write_text(process.stderr)
        process.check_returncode()
        resources = None
        if workers is not None or args.dbcache_mib is not None:
            metadata = [line.removeprefix('INDEXED_FIXTURE ') for line in process.stderr.splitlines()
                        if line.startswith('INDEXED_FIXTURE ')]
            if len(metadata) != 1:
                raise RuntimeError('Missing benchmark resource metadata')
            resources = json.loads(metadata[0])
            if (resources['script_workers'] != (2 if workers is None else workers)
                    or resources['dbcache_mib'] != (512 if args.dbcache_mib is None else args.dbcache_mib)
                    or resources['transactions'] < args.count or resources['non_coinbase_inputs'] < 0
                    or resources['fixture_bytes'] != blocks.stat().st_size):
                raise RuntimeError('Benchmark resource/input metadata mismatch')
        sample = measurements(process.stdout, args.count, args.mode, args.tip, args.utxo)
        if args.require_candidate_reuse and name == 'candidate' and sample[args.mode]['yespower_calls']:
            raise RuntimeError('Candidate repeated an indexed block proof')
        row = {'variant': name, 'command': command, 'resources': resources, 'measurements': sample}
        report['runs'].append(row)
        (args.work_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(row), flush=True)
    if any(digest(Path(path)) != value for path, value in hashes.items()):
        raise RuntimeError('Input/executable changed during comparison')
    medians = {name: statistics.median(r['measurements'][args.mode]['seconds'] for r in report['runs']
                                      if r['variant'] == name) for name in binaries}
    report['median_block_seconds'] = medians
    report['component_speedup'] = medians['baseline'] / medians['candidate']
    (args.work_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(medians), flush=True)


if __name__ == '__main__':
    main()
