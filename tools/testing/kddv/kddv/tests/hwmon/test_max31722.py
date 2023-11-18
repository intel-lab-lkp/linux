#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

from kddv.core import SPIDriverTest
from kddv.core.consts import CFG_REG_MASK
from . import HwMonDriver
import errno

MAX31722_REG_CFG = 0x00
MAX31722_REG_TEMP_LSB = 0x01
MAX31722_MODE_CONTINUOUS = 0x00
MAX31722_RESOLUTION_12BIT = 0x06

class TestMax31722(SPIDriverTest, HwMonDriver):
    name = 'max31722'

    @property
    def configs(self):
        return { CFG_REG_MASK: 0x7f }

    def test_device_probe(self):
        with self.assertRaisesFault():
            with self.device() as dev:
                self.assertRegEqual(MAX31722_REG_CFG, MAX31722_RESOLUTION_12BIT)

    def test_read_temp_input(self):
        with self.device() as dev:
            self.write_regs(MAX31722_REG_TEMP_LSB, [0x12, 0x34])
            temp = self.hwmon_read_temp_input(dev)
            self.assertEqual(temp, int(0x3412 * 125 / 32))

            self.trigger_io_fault()
            with self.assertRaises(OSError) as cm:
                self.hwmon_read_temp_input(dev)
            self.assertEqual(cm.exception.errno, errno.EIO)
