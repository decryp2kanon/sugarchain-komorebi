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
    result = {r['phase']: {'seconds': float(r['seconds']), 'yespower_calls': int(r['yespower_calls']),
                          'yespower_cpu_ns': int(r['yespower_cpu_ns'])} for r in rows}
    for row in rows:
        for field in ('chainstate_write_events', 'tip_log_events', 'log_bytes'):
            if field in row:
                value = int(row[field])
                if value < 0:
                    raise ValueError('Invalid diagnostic counter')
                result[row['phase']][field] = value
    return result


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
    p.add_argument('--mempool-mb', type=int, help='same borrowable mempool budget in decimal MB (5..16384)')
    logging = p.add_mutually_exclusive_group()
    logging.add_argument('--log-coindb', action='store_true', help='count write events; adds logging overhead to BOTH arms')
    logging.add_argument('--log-info-file', action='store_true', help='ordinary file logs and tip/byte counts on BOTH arms')
    p.add_argument('--timeout', type=int, default=1200)
    p.add_argument('--work-dir', type=Path, required=True, help='must not exist')
    args = p.parse_args()
    if not 1 <= args.count <= 100000 or args.timeout <= 0:
        p.error('count must be 1..100000 and timeout positive')
    if args.dbcache_mib is not None and not 4 <= args.dbcache_mib <= 16384:
        p.error('dbcache-mib must be 4..16384')
    if args.mempool_mb is not None and not 5 <= args.mempool_mb <= 16384:
        p.error('mempool-mb must be 5..16384')
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
        logged = args.log_coindb or args.log_info_file
        resources_requested = workers is not None or args.dbcache_mib is not None or args.mempool_mb is not None or logged
        if resources_requested:
            command.append(str(2 if workers is None else workers))
        if args.dbcache_mib is not None or args.mempool_mb is not None or logged:
            command.append(str(512 if args.dbcache_mib is None else args.dbcache_mib))
        if args.mempool_mb is not None or logged:
            command.append(str(300 if args.mempool_mb is None else args.mempool_mb))
        if args.log_coindb:
            command.append('--log-coindb')
        if args.log_info_file:
            command.append('--log-info-file')
        process = subprocess.run(command, capture_output=True,
                                 text=True, timeout=args.timeout)
        (args.work_dir / f'{number}-{name}.stdout').write_text(process.stdout)
        (args.work_dir / f'{number}-{name}.stderr').write_text(process.stderr)
        process.check_returncode()
        resources = None
        metadata = [line.removeprefix('INDEXED_FIXTURE ') for line in process.stderr.splitlines()
                    if line.startswith('INDEXED_FIXTURE ')]
        if resources_requested or metadata:
            if len(metadata) != 1:
                raise RuntimeError('Missing benchmark resource metadata')
            resources = json.loads(metadata[0])
            if (resources['script_workers'] != (2 if workers is None else workers)
                    or resources['dbcache_mib'] != (512 if args.dbcache_mib is None else args.dbcache_mib)
                    or resources['transactions'] < args.count or resources['non_coinbase_inputs'] < 0
                    or resources['fixture_bytes'] != blocks.stat().st_size):
                raise RuntimeError('Benchmark resource/input metadata mismatch')
            if args.mempool_mb is not None or logged or 'coins_cache_bytes' in resources:
                if (resources.get('mempool_max_bytes') != (300 if args.mempool_mb is None else args.mempool_mb) * 1_000_000
                        or resources.get('coindb_logging') != args.log_coindb):
                    raise RuntimeError('Mempool/logging metadata mismatch')
                total = (512 if args.dbcache_mib is None else args.dbcache_mib) * 1024**2
                block_tree = min(total // 8, 2 * 1024**2)
                coins_db = min((total - block_tree) // 2, 8 * 1024**2)
                if (resources.get('block_tree_cache_bytes') != block_tree
                        or resources.get('coins_db_cache_bytes') != coins_db
                        or resources.get('coins_cache_bytes') != total - block_tree - coins_db):
                    raise RuntimeError('Actual DB cache split mismatch')
                if args.log_info_file and resources.get('info_file_logging') is not True:
                    raise RuntimeError('File logging metadata mismatch')
        sample = measurements(process.stdout, args.count, args.mode, args.tip, args.utxo)
        if args.log_coindb and any('chainstate_write_events' not in phase for phase in sample.values()):
            raise RuntimeError('Missing requested write-event diagnostics')
        if args.log_info_file and (sample[args.mode].get('log_bytes', 0) <= 0
                                  or sample[args.mode].get('tip_log_events', 0) <= 0):
            raise RuntimeError('Missing requested file/tip logging')
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
    report['actual_cache_budgets_verified'] = all(r['resources'] is not None
        and 'coins_cache_bytes' in r['resources'] for r in report['runs'])
    (args.work_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(medians), flush=True)


if __name__ == '__main__':
    main()
