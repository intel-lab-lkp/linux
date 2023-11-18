#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

from kddv.core import SPIDriverTest
from . import MTDDriver

MCHP23K256_TEST_DATA = [0x78] * 16

class TestMCHP23K256(SPIDriverTest, MTDDriver):
    name = "mchp23k256"

    @property
    def bpf(self):
        return f"mtd-{self.name}"

    def test_device_probe(self):
        with self.assertRaisesFault():
            with self.device() as _:
                pass

    def test_device_size(self):
        with self.device() as dev:
            size = self.mtd_read_attr(dev, 'size')
            self.assertEqual(size, '32768')

    def test_read_data(self):
        with self.device() as dev:
            self.write_regs(0x00, MCHP23K256_TEST_DATA)
            data = self.mtd_read_bytes(dev, 16)
            self.assertEqual(data, bytes(MCHP23K256_TEST_DATA))

    def test_write_data(self):
        with self.device() as dev:
            self.write_regs(0x00, [0] * 16)
            self.mtd_write_bytes(dev, bytes(MCHP23K256_TEST_DATA))
            self.assertRegsEqual(0x00, MCHP23K256_TEST_DATA)
