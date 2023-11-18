#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import logging
import subprocess
from pathlib import Path

from .device import Device
from .environ import environ

logger = logging.getLogger(__name__)

class Driver(object):
    def __init__(self, p):
        self.bus = p.bus
        self.domain_nr = p.domain_nr
        self.bus_id = p.bus_id
        self.parent_bus = p.parent_bus
        self.driver = p.driver_name
        self.module = p.module_name
        self.depmod = p.dependencies
        self.path = Path(f"/sys/bus/{self.bus}/drivers/{self.driver}")

    def write_ctrl(self, spath, val):
        path = self.path / spath
        logger.debug(f"write '{val}' to {path}")
        return path.write_text(val)

    def disable_autoprobe(self):
        logger.debug(f"disable {self.bus} drivers autoprobe")
        path = Path(f"/sys/bus/{self.bus}/drivers_autoprobe")
        path.write_text("0")

    def enable_autoprobe(self):
        logger.debug(f"enable {self.bus} drivers autoprobe")
        path = Path(f"/sys/bus/{self.bus}/drivers_autoprobe")
        path.write_text("1")

    def probe_depmod(self):
        if not self.depmod:
            return
        for mod in self.depmod:
            logger.debug(f'modprobe {mod}')
            subprocess.check_output(["/sbin/modprobe", mod])

    def probe_module(self):
        self.probe_depmod()
        logger.debug(f'modprobe {self.module}')
        subprocess.check_output(
            ["/sbin/modprobe", self.module], stderr=subprocess.STDOUT
        )
        environ.notify_insmod(self.module)

    def remove_mdule(self):
        logger.debug(f'rmmod {self.module}')
        environ.notify_rmmod()
        subprocess.check_output(["/sbin/rmmod", self.module])

    def setup(self):
        self.disable_autoprobe()
        self.probe_module()

    def teardown(self):
        self.remove_mdule()
        self.enable_autoprobe()

    def bind(self, devid):
        if os.path.exists(self.path / devid):
            return
        self.write_ctrl("bind", devid)

    def unbind(self, devid):
        self.write_ctrl("unbind", devid)

    def device(self, addr, devid):
        return Device.create(self.bus, self, self.bus_id, addr, devid)
