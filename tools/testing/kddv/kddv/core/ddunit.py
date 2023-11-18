#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging
import unittest

from .model import DriverModel

logger = logging.getLogger(__name__)

class DriverTest(unittest.TestCase, DriverModel):
    def __init__(self, methodName=None):
        super().__init__(methodName)
        DriverModel.__init__(self)

    def setUp(self):
        super().setUp()
        try:
            self.driver.setup()
        except:
            self.skipTest(f"Module {self.module_name} not found")
        self.mockup.load()

    def tearDown(self):
        self.mockup.unload()
        self.driver.teardown()
        super().tearDown()

    def assertRegEqual(self, reg, data, msg=None):
        value = self.read_reg(reg)
        self.assertEqual(value, data, msg)

    def assertRegBitsEqual(self, reg, data, mask, msg=None):
        value = self.read_reg(reg)
        self.assertEqual(value & mask, data & mask, msg)

    def assertRegsEqual(self, reg, data, msg=None):
        value = self.read_regs(reg, len(data))
        self.assertListEqual(value, data, msg)
