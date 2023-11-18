#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import sys
import fnmatch
import argparse
import unittest

from kddv.core.ddunit import DriverTest
from kddv.core.environ import environ
from . import utils

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _list_suite(suite, tfilter=None):
    for t in suite:
        if isinstance(t, unittest.TestSuite):
            _list_suite(t, tfilter)
        elif isinstance(t, DriverTest):
            id = t.id()
            if tfilter and not any(fnmatch.fnmatch(id, f) for f in tfilter):
                continue
            print(f"kddv.tests.{id}")
        else:
            return None

def list_suite(args):
    args.filter = [f"*{f}*" for f in args.filter]
    loader = unittest.defaultTestLoader
    suites = loader.discover(os.path.join(ROOT_DIR, 'tests'))
    _list_suite(suites, args.filter)
    return 0

def main() -> None:
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--log-level", type=str, default=None,
        choices=utils.LOG_LEVELS, help="Log Level"
    )
    parser.add_argument(
        "--list", action='store_true', default=False,
        help="List test cases",
    )
    parser.add_argument(
        "--kmemleak", action='store_true', default=False,
        help="Enable kmemeleak check",
    )
    parser.add_argument(
        "--fault-inject", type=str, default=None,
        choices=["slab", "page", "all"],
        help="Enable fault inject features",
    )
    parser.add_argument("--filter", nargs="+", default=[],)

    args, argv = parser.parse_known_args(sys.argv)
    if args.log_level:
        utils.setup_logger(args.log_level)
    if args.list:
        return list_suite(args)
    if args.kmemleak:
        environ.enable_kmemleak()
    if args.fault_inject:
        environ.enable_fault_inject(args.fault_inject)

    unittest.main(verbosity=2, module=None, argv=argv)

if __name__ == "__main__":
    main()
