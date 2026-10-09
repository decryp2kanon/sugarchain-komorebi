#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Fast-only compact indexes survive competing branches and legacy restarts."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class CompactBlockMapTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.extra_args = [["-fast-startup=1"], ["-fast-startup=1"]]

    def run_test(self):
        a, b = self.nodes
        self.generate(a, 201)
        self.disconnect_nodes(0, 1)
        short_branch = self.generate(a, 5, sync_fun=self.no_op)
        long_branch = self.generate(b, 8, sync_fun=self.no_op)
        self.connect_nodes(0, 1)
        self.sync_all()
        assert_equal(a.getbestblockhash(), long_branch[-1])
        assert a.verifychain(3, 20)

        self.disconnect_nodes(0, 1)
        a.invalidateblock(long_branch[0])
        assert_equal(a.getbestblockhash(), short_branch[-1])
        a.reconsiderblock(long_branch[0])
        assert_equal(a.getbestblockhash(), long_branch[-1])
        expected = a.getblockchaininfo()
        self.stop_nodes()

        # The compact layout is process-local: both existing and new DB
        # entries must remain readable by the unchanged legacy startup path.
        for mode in ["0", "1"]:
            self.start_node(0, [f"-fast-startup={mode}"])
            actual = a.getblockchaininfo()
            for key in ["blocks", "headers", "bestblockhash", "chainwork"]:
                assert_equal(actual[key], expected[key])
            assert_equal(a.getblockhash(202), long_branch[0])
            assert a.verifychain(3, 20)
            self.stop_node(0)


if __name__ == '__main__':
    CompactBlockMapTest(__file__).main()
