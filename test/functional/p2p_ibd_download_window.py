#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Exercise the opt-in lookahead boundary and unchanged stall recovery/backoff."""
from p2p_ibd_stalling import P2PIBDStallingTest


class IBDDownloadWindowTest(P2PIBDStallingTest):
    download_window = 4096

    def set_test_params(self):
        super().set_test_params()
        self.supports_cli = False
        self.extra_args = [['-conf=bitcoin.conf', '-disablewallet=1',
                            '-maxibdblocksinflight=128', '-maxibddownloadwindow=4096']]

    def run_test(self):
        super().run_test()


if __name__ == '__main__':
    IBDDownloadWindowTest(__file__).main()
