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
from .environ import environ
from .failnth import FaultIterator

logger = logging.getLogger(__name__)

class _AssertRaisesFaultContext(unittest.case._AssertRaisesContext):
    def __enter__(self):
        environ.enter_fault_inject()
        return self

    def __exit__(self, exc_type, exc_value, tb):
        if not environ.exit_fault_inject():
            return False
        if exc_type is None:
            return True
        if issubclass(exc_type, self.expected):
            return True
        if issubclass(exc_type, AssertionError):
            return True
        return super().__exit__(exc_type, exc_value, tb)

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
        environ.setup()

    def tearDown(self):
        environ.teardown()
        self.mockup.unload()
        self.driver.teardown()
        super().tearDown()

    def _callTestMethod(self, method):
        fault = FaultIterator()
        for nth in iter(fault):
            logger.debug(f"fault inject: nth={nth}")
            method()
        self.assertFault()

    def assertRegEqual(self, reg, data, msg=None):
        value = self.read_reg(reg)
        self.assertEqual(value, data, msg)

    def assertRegBitsEqual(self, reg, data, mask, msg=None):
        value = self.read_reg(reg)
        self.assertEqual(value & mask, data & mask, msg)

    def assertRegsEqual(self, reg, data, msg=None):
        value = self.read_regs(reg, len(data))
        self.assertListEqual(value, data, msg)

    def assertFault(self):
        msg = environ.check_failure()
        if msg:
            raise self.failureException(msg)

    def assertRaisesFault(self, *args, **kwargs):
        context = _AssertRaisesFaultContext(OSError, self)
        try:
            return context.handle('assertRaises', args, kwargs)
        finally:
            context = None
