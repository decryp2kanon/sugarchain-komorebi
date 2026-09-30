#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Run upstream's window/stall/backoff regression with the opt-in IBD budget.

The original test still covers the unchanged default. This variant exercises
1024-block window exhaustion, staller eviction/backoff and recovery at 128.
"""
from p2p_ibd_stalling import P2PIBDStallingTest


class IBDDownloadLimitsTest(P2PIBDStallingTest):
    def add_options(self, parser):
        parser.add_argument('--inflight-limit', default='128')

    def set_test_params(self):
        super().set_test_params()
        self.supports_cli = False
        self.extra_args = [['-conf=bitcoin.conf', '-disablewallet=1',
                            '-maxibdblocksinflight=' + self.options.inflight_limit]]

    def run_test(self):
        super().run_test()


if __name__ == '__main__':
    IBDDownloadLimitsTest(__file__).main()
