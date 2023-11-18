#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import sys
import logging
import argparse
import unittest
import subprocess

from kddv.core.mockup import Mockup
from kddv.core.ddunit import DriverTest
from kddv.core.buses import *
from . import utils

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

logger = logging.getLogger()

def search(suite, bus: str, devid: str):
    mdrv = None
    for t in suite:
        if isinstance(t, unittest.TestSuite):
            driver = search(t, bus, devid)
            if driver:
                if driver.device_id == devid:
                    return driver
                mdrv = driver
        elif isinstance(t, DriverTest):
            if not hasattr(t, 'bus') or not hasattr(t, 'device_id'):
                logger.debug(f"not a driver test case: {t}")
                continue
            if t.bus != bus:
                continue
            if t.device_id == devid:
                return t
            if  t.driver_name == devid:
                mdrv = t
        else:
            return mdrv

def do_mockup_device(t):
    mock = Mockup.create(t.bus, t)
    try:
        subprocess.check_output(
            ["/sbin/modprobe", t.module_name], stderr=subprocess.STDOUT
        )
    except:
        logger.warning(f"Module {t.module_name} not found")
        sys.exit(1)

    mock.load()
    logger.warning(f"create {t.bus} device {t.device_id} success!")

def do_remove_device(t):
    mock = Mockup.create(t.bus, t)
    try:
        mock.unload()
        logger.warning(f"remove {t.bus} device {t.device_id} success!")
    except:
        logger.warning(f"{t.bus} device {t.device_id} not found")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--bus", "-b", type=str, required=True,
        choices=["i2c", "spi", "pci", "platform"], help="Bus Types"
    )
    parser.add_argument(
        "--devid", "-d", type=str, required=True, help="Device ID"
    )
    parser.add_argument(
        "--log-level", "-l", type=str, default=None,
        choices=utils.LOG_LEVELS, help="Log level"
    )
    parser.add_argument(
        "--remove", "-r", action='store_true', default=False,
        help="Remove device",
    )
    args = parser.parse_args()

    if args.log_level:
        utils.setup_logger(args.log_level)

    loader = unittest.defaultTestLoader
    suites = loader.discover(os.path.join(ROOT_DIR, 'tests'))
    driver = search(suites, args.bus, args.devid)
    if driver is None:
        logger.error(f"{args.bus} device {args.devid} not support")
        sys.exit(1)

    if not args.remove:
        do_mockup_device(driver)
    else:
        do_remove_device(driver)

    sys.exit(0)

if __name__ == "__main__":
    main()
