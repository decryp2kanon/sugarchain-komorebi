#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Manual Linux requested-block pipeline benchmark using fresh regtest data.

Set PROBE_RESULT, BITCOIND and BITCOINCLI; pass the build's --configfile and a
fresh --tmpdir. PROBE_WINDOW selects the opt-in lookahead, PROBE_COUNT the block
count, PROBE_PEERS the responder count, and PROBE_DELAYS_MS comma-separated
response delays. Source preparation/serialization precedes the acceptance timer.
This helper does not enforce a CPU/RAM budget: apply external resource controls.

I/O counters end after the explicit final sync/consistency check, before shutdown.
The child high-water mark after shutdown assumes the daemon is the only sizable
child; CLI mode is disabled. Restarted-process RSS is also retained separately.
See doc/ibd-download-window.md for scope, protocol and measurement limitations.
"""
import hashlib
import json
import os
import resource
import sys
import threading
import time
from pathlib import Path
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'test' / 'functional'))
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_block, msg_headers, MSG_BLOCK, MSG_TYPE_MASK
from test_framework.p2p import P2PInterface, NetworkThread
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

class Supplier(P2PInterface):

    def __init__(self, index, payloads, delay_ms):
        super().__init__()
        self.index = index
        self.payloads = payloads
        self.delay = delay_ms / 1000
        self.released = False
        self.pending = []
        self.events = []
        self.sent = 0

    def on_getheaders(self, message):
        pass

    def on_getdata(self, message):
        wanted = [i.hash for i in message.inv if i.type & MSG_TYPE_MASK == MSG_BLOCK]
        if not wanted:
            return
        self.events.append({'kind': 'request', 't': time.monotonic(), 'count': len(wanted)})
        if self.released:
            self.schedule(wanted)
        else:
            self.pending.append(wanted)

    def schedule(self, wanted):

        def deliver():
            if not self.is_connected:
                return
            data = b''.join((self.payloads[h] for h in wanted))
            self._transport.write(data)
            self.sent += len(wanted)
            self.events.append({'kind': 'enqueue', 't': time.monotonic(), 'count': len(wanted), 'bytes': len(data)})
        NetworkThread.network_event_loop.call_later(self.delay, deliver)

    def release(self):
        self.released = True
        for wanted in self.pending:
            self.schedule(wanted)
        self.pending.clear()

class Probe(BitcoinTestFramework):

    def set_test_params(self):
        if not Path('/proc/self/stat').is_file():
            raise RuntimeError('This benchmark requires Linux /proc resource counters')
        if not os.environ.get('PROBE_RESULT'):
            raise RuntimeError('Set PROBE_RESULT to an output JSON path')
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False
        self.extra_args = [['-conf=bitcoin.conf', '-disablewallet=1', '-checkblockindex=0', '-fast-startup=0', '-parpow=8', '-dbcache=512', '-debug=0', '-maxibdblocksinflight=' + os.getenv('PROBE_LIMIT', '128')]]
        if os.getenv('PROBE_WINDOW'):
            self.extra_args[0].append('-maxibddownloadwindow=' + os.environ['PROBE_WINDOW'])

    def run_test(self):
        n = self.nodes[0]
        count = int(os.getenv('PROBE_COUNT', '20000'))
        num = int(os.getenv('PROBE_PEERS', '8'))
        delay = float(os.getenv('PROBE_DELAY_MS', '100'))
        if not 1 <= count <= 1000000 or not 1 <= num <= 8:
            raise ValueError('Use 1..1000000 blocks and 1..8 peers')
        info = n.getblockchaininfo()
        tip = n.getblockheader(info['bestblockhash'])
        prev = int(info['bestblockhash'], 16)
        blocks = []
        payloads = {}
        for i in range(count):
            b = create_block(prev, create_coinbase(i + 1), tip['time'] + i + 1)
            b.solve()
            prev = b.hash_int
            blocks.append(b)
        delays = [float(x) for x in os.environ.get('PROBE_DELAYS_MS', str(delay)).split(',')]
        assert len(delays) in (1, num) and min(delays) >= 0 and (max(delays) < 2000)
        peers = []
        for i in range(num):
            peer = n.add_outbound_p2p_connection(Supplier(i, payloads, delays[i] if len(delays) > 1 else delays[0]), p2p_idx=i, connection_type='outbound-full-relay', supports_v2_p2p=False)
            peers.append(peer)
        for b in blocks:
            payloads[b.hash_int] = peers[0].build_message(msg_block(b))
        for peer in peers:
            for pos in range(0, count, 2000):
                peer.send_and_ping(msg_headers([CBlockHeader(b) for b in blocks[pos:pos + 2000]]))
        assert_equal(n.getblockchaininfo()['headers'], count)
        assert_equal(n.getblockcount(), 0)
        assert n.getblockchaininfo()['initialblockdownload']
        pid = n.process.pid
        sampled = []
        stop = threading.Event()
        ticks = os.sysconf('SC_CLK_TCK')

        def snapshot(for_pid=pid):
            st = Path(f'/proc/{for_pid}/stat').read_text().rsplit(')', 1)[1].split()
            out = {'t': time.monotonic(), 'cpu_s': (int(st[11]) + int(st[12])) / ticks}
            for line in Path(f'/proc/{for_pid}/status').read_text().splitlines():
                (k, _, v) = line.partition(':')
                if k in ('VmRSS', 'VmHWM', 'RssAnon', 'RssFile', 'VmSwap'):
                    out[k] = int(v.split()[0])
            out['io'] = {line.split(':')[0]: int(line.split()[1]) for line in Path(f'/proc/{for_pid}/io').read_text().splitlines()}
            return out

        def monitor():
            while not stop.wait(0.1):
                try:
                    sampled.append(snapshot())
                except FileNotFoundError:
                    return
        before = snapshot()
        thread = threading.Thread(target=monitor)
        thread.start()
        begin = time.monotonic()
        progress = []
        for peer in peers:
            NetworkThread.network_event_loop.call_soon_threadsafe(peer.release)
        try:
            while True:
                chain = n.getblockchaininfo()
                data = n.getpeerinfo()
                now = time.monotonic()
                progress.append({'t': now, 'height': chain['blocks'], 'inflight': sum((len(p.get('inflight', [])) for p in data)), 'peers': len(data)})
                if chain['blocks'] == count:
                    break
                if now - begin > 180:
                    raise TimeoutError('owned probe interval exceeded 180 seconds')
                time.sleep(0.02)
            elapsed = time.monotonic() - begin
            after = snapshot()
            assert_equal(chain['bestblockhash'], f'{prev:064x}')
            assert_equal(len(data), num)
            flush = time.monotonic()
            utxo = n.gettxoutsetinfo('none')
            flush = time.monotonic() - flush
            assert_equal(utxo['height'], count)
            assert_equal(utxo['bestblock'], chain['bestblockhash'])
            assert n.verifychain(3, 100)
            after_sync = snapshot()
        finally:
            stop.set()
            thread.join()
        result = {'measurement_version': 2, 'node_pid': pid, 'io_through_final_sync': {k: after_sync['io'][k] - before['io'][k] for k in before['io']}, 'node_cpu_through_final_sync_s': after_sync['cpu_s'] - before['cpu_s'], 'count': count, 'num_peers': num, 'response_delay_ms': delay, 'response_delays_ms': delays, 'limit': os.getenv('PROBE_LIMIT', '128'), 'seconds': elapsed, 'blocks_per_s': count / elapsed, 'node_cpu_s': after['cpu_s'] - before['cpu_s'], 'node_cpu_percent': 100 * (after['cpu_s'] - before['cpu_s']) / elapsed, 'peak_rss_kib': max([before['VmHWM'], after['VmHWM'], after_sync['VmHWM']] + [s['VmHWM'] for s in sampled]), 'flush_utxo_s': flush, 'ibd_at_end': chain['initialblockdownload'], 'tip': chain['bestblockhash'], 'progress': progress, 'samples': sampled, 'suppliers': [{'index': p.index, 'sent': p.sent, 'events': p.events} for p in peers], 'fixture_payload_sha256': hashlib.sha256(b''.join((payloads[b.hash_int] for b in blocks))).hexdigest()}
        shutdown_start = time.monotonic()
        self.stop_node(0)
        result['shutdown_seconds'] = time.monotonic() - shutdown_start
        result['original_lifetime_child_peak_kib'] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        restart_start = time.monotonic()
        self.start_node(0)
        result['restart_seconds'] = time.monotonic() - restart_start
        again = n.getblockchaininfo()
        assert_equal(again['blocks'], count)
        assert_equal(again['bestblockhash'], chain['bestblockhash'])
        assert n.verifychain(3, 100)
        restarted = snapshot(n.process.pid)
        result['restarted_peak_rss_kib'] = restarted['VmHWM']
        result['peak_rss_kib'] = max(result['peak_rss_kib'], result['original_lifetime_child_peak_kib'], restarted['VmHWM'])
        result['total_accept_and_sync_seconds'] = elapsed + flush
        Path(os.environ['PROBE_RESULT']).write_text(json.dumps(result, indent=2) + '\n')
        self.log.info('Probe %.3f s, %.1f blocks/s, node CPU %.1f%%', elapsed, count / elapsed, result['node_cpu_percent'])
if __name__ == '__main__':
    Probe(__file__).main()
