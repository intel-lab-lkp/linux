#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import logging

from pathlib import Path

logger = logging.getLogger(__name__)

class MTD(object):
    def __init__(self, path):
        mtd = next(path.glob("mtd/mtd*")).name
        self.cdev = Path(f"/sys/class/mtd/{mtd}")
        self.rdev = f"/dev/{mtd}"

    def read_bytes(self, len, offset = 0):
        with open(self.rdev, "rb") as dev:
            if offset:
                dev.seek(offset)
            return dev.read(len)

    def write_bytes(self, data, offset = 0):
        with open(self.rdev, "wb") as dev:
            if offset:
                dev.seek(offset)
            dev.write(data)

    def read_attr(self, attr):
        path = self.cdev / attr
        logger.debug(f"read from {path}")
        if not os.path.exists(path):
            return f"attr '{attr}' not exists"
        return path.read_text().rstrip()

    def write_attr(self, attr, val):
        path = self.cdev / attr
        if not os.path.exists(path):
            return f"attr '{attr}' not exists"
        logger.debug(f"write '{val}' to {path}")
        return path.write_bytes(val.encode())

class MTDDriver(object):
    def mtd_read_attr(self, dev, attr):
        mtddev = MTD(dev.path)
        return mtddev.read_attr(attr)

    def mtd_write_attr(self, dev, attr, val):
        mtddev = MTD(dev.path)
        return mtddev.write_attr(attr)

    def mtd_read_bytes(self, dev, len):
        mtddev = MTD(dev.path)
        return mtddev.read_bytes(len)

    def mtd_write_bytes(self, dev, data):
        mtddev = MTD(dev.path)
        return mtddev.write_bytes(data)
