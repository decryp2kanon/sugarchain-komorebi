#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Read-only Linux IBD RPC/process sampling. Never starts/stops/reconfigures a node.

Cookie authentication is handled by the supplied CLI. Output contains no RPC
credentials or peer addresses. A reused PID terminates observation, and errors
are recorded rather than being represented as zero progress.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def process_stat(text):
    # comm may contain spaces/parentheses; fields after its final ')' start at 3.
    fields = text[text.rindex(')') + 2:].split()
    return {
        'state': fields[0],
        'start_ticks': int(fields[19]),
        'cpu_ticks': int(fields[11]) + int(fields[12]),
        'rss_pages': int(fields[21]),
        'major_faults': int(fields[9]),
    }


def peer_summary(peers):
    fields = ('id', 'connection_type', 'inbound', 'presynced_headers', 'synced_headers',
              'synced_blocks', 'pingtime', 'bytessent', 'bytesrecv', 'last_block')
    return [dict({key: peer[key] for key in fields if key in peer},
                 inflight_count=len(peer.get('inflight', []))) for peer in peers]


def rpc(command, method):
    result = subprocess.run(command + [method], capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise RuntimeError(f'{method}: CLI exit {result.returncode}: {result.stderr.strip()}')
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--datadir', type=Path, required=True)
    parser.add_argument('--rpcport', type=int, required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new JSONL file; never overwritten')
    parser.add_argument('--interval', type=float, default=10)
    parser.add_argument('--samples', type=int, default=0, help='zero observes until the process exits')
    args = parser.parse_args()
    if args.pid <= 0 or not 1 <= args.rpcport <= 65535 or args.interval < 1 or args.samples < 0:
        parser.error('invalid PID/port/interval/sample count')
    command = [str(args.cli.resolve(strict=True)), f'-datadir={args.datadir.resolve(strict=True)}',
               f'-rpcport={args.rpcport}']
    proc = Path('/proc') / str(args.pid)
    identity = process_stat((proc / 'stat').read_text())['start_ticks']
    started = time.monotonic()
    with args.output.open('x', buffering=1) as output:
        output.write(json.dumps({'type': 'metadata', 'pid': args.pid, 'start_ticks': identity,
                                 'clock_ticks': os.sysconf('SC_CLK_TCK'),
                                 'page_bytes': os.sysconf('SC_PAGE_SIZE'),
                                 'started_unix': time.time(), 'read_only': True}) + '\n')
        count = 0
        while args.samples == 0 or count < args.samples:
            deadline = time.monotonic() + args.interval
            row = {'type': 'sample', 'unix': time.time(), 'elapsed': time.monotonic() - started}
            try:
                row['process'] = process_stat((proc / 'stat').read_text())
                if row['process']['start_ticks'] != identity:
                    raise ProcessLookupError('PID was reused')
                if row['process']['state'] in ('Z', 'X'):
                    raise ProcessLookupError('Process exited; awaiting parent reaping')
                row['io'] = {key: int(value) for key, value in
                             (line.split(':', 1) for line in (proc / 'io').read_text().splitlines())}
                row['chain'] = rpc(command, 'getblockchaininfo')
                row['peers'] = peer_summary(rpc(command, 'getpeerinfo'))
                row['network'] = rpc(command, 'getnettotals')
            except (FileNotFoundError, ProcessLookupError) as error:
                row['ended'] = str(error)
                output.write(json.dumps(row) + '\n')
                break
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                row['error'] = str(error)
            output.write(json.dumps(row) + '\n')
            count += 1
            if args.samples and count >= args.samples:
                break
            time.sleep(max(0, deadline - time.monotonic()))


if __name__ == '__main__':
    main()
