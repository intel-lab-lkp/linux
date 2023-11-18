#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

from .driver import Driver
from .mockup import Mockup

class DriverModel(object):
    bus = None
    name = None
    addr = 0x00

    def __init__(self):
        self.driver = Driver(self)
        self.mockup = Mockup.create(self.bus, self)

    @property
    def driver_name(self):
        return self.name

    @property
    def module_name(self):
        return self.name

    @property
    def dependencies(self):
        """List of module dependencies by running tests."""
        return None

    @property
    def domain_nr(self):
        return 0

    @property
    def bus_id(self):
        return 0

    @property
    def parent_bus(self):
        return None

    @property
    def bpf(self):
        return None

    @property
    def address(self):
        return self.addr

    @property
    def device_id(self):
        return self.name

    @property
    def regshift(self):
        return 0

    @property
    def regbytes(self):
        return 1

    @property
    def valbytes(self):
        return 1

    @property
    def regmaps(self):
        return {}

    def device(self):
        return self.driver.device(self.address, self.device_id)

    def read_reg(self, addr):
        return self.mockup.read_reg(addr)

    def read_regs(self, addr, len):
        return self.mockup.read_regs(addr, len)

    def write_reg(self, addr, val):
        self.mockup.write_reg(addr, val)

    def write_regs(self, addr, data):
        self.mockup.write_regs(addr, data)
