#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Deterministic parser/error tests; no node or network required."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('observe_ibd', Path(__file__).with_name('observe-ibd.py'))
observe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observe)


class ObservationTests(unittest.TestCase):
    def test_proc_fields_and_parentheses(self):
        fields = ['S'] + [str(i) for i in range(4, 25)]
        actual = observe.process_stat('123 (name with ) brackets) ' + ' '.join(fields))
        self.assertEqual(actual, {'state': 'S', 'start_ticks': 22, 'cpu_ticks': 29,
                                  'rss_pages': 24, 'major_faults': 12})

    def test_peer_summary_preserves_missing_progress(self):
        actual = observe.peer_summary([{'id': 1, 'addr': 'private', 'inflight': [10, 11]},
                                       {'id': 2, 'presynced_headers': 0}])
        self.assertEqual(actual, [{'id': 1, 'inflight_count': 2},
                                  {'id': 2, 'presynced_headers': 0, 'inflight_count': 0}])

    def test_rpc_read_only_command(self):
        with patch.object(observe.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '{"blocks": 12}', '')) as run:
            self.assertEqual(observe.rpc(['cli', '-datadir=test'], 'getblockchaininfo'), {'blocks': 12})
            self.assertEqual(run.call_args.args[0], ['cli', '-datadir=test', 'getblockchaininfo'])
            self.assertEqual(run.call_args.kwargs['timeout'], 10)

    def test_rpc_errors_are_not_zero_progress(self):
        with patch.object(observe.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'unavailable')):
            with self.assertRaises(RuntimeError):
                observe.rpc(['cli'], 'getpeerinfo')

        with patch.object(observe.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'broken', '')):
            with self.assertRaises(json.JSONDecodeError):
                observe.rpc(['cli'], 'getpeerinfo')
        with patch.object(observe.subprocess, 'run', side_effect=subprocess.TimeoutExpired('cli', 10)):
            with self.assertRaises(subprocess.TimeoutExpired):
                observe.rpc(['cli'], 'getpeerinfo')

    @unittest.skipUnless(Path('/proc/self/stat').exists(), 'Linux process observation')
    def test_exited_process_ends_without_rpc_or_waiting_for_reaper(self):
        child = subprocess.Popen([sys.executable, '-c', 'pass'])
        try:
            deadline = time.monotonic() + 5
            while observe.process_stat(Path(f'/proc/{child.pid}/stat').read_text())['state'] != 'Z':
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / 'result.jsonl'
                subprocess.run([sys.executable, str(Path(__file__).with_name('observe-ibd.py')),
                                '--cli', '/bin/false', '--datadir', directory,
                                '--rpcport', '1', '--pid', str(child.pid), '--output', str(output)],
                               check=True, timeout=5)
                rows = [json.loads(line) for line in output.read_text().splitlines()]
                self.assertEqual(len(rows), 2)
                self.assertIn('awaiting parent reaping', rows[-1]['ended'])
                self.assertNotIn('chain', rows[-1])
        finally:
            child.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
