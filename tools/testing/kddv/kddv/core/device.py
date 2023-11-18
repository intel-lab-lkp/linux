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

class Device(object):
    bus = None

    def __init__(self, bus, driver, busid, addr, devid):
        self.bus = bus
        self.driver = driver
        self.busid = busid
        self.addr = addr
        self.devid = devid
        self.status = False
        self.path = Path(f"/sys/bus/{self.bus}/devices/{self.device_id}")

    def __enter__(self):
        self.bind()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.unbind()

    def __del__(self):
        self.unbind()

    @classmethod
    def create(cls, bus, driver, busid, addr, devid):
        for subclass in cls.__subclasses__():
            if subclass.bus == bus:
                return subclass(bus, driver, busid, addr, devid)
        return cls(bus, driver, busid, addr, devid)

    @property
    def device_id(self):
        return self.devid

    def bind(self):
        if self.status is True:
            return
        self.status = True
        self.driver.bind(self.device_id)

    def unbind(self):
        if self.status is False:
            return
        try:
            self.driver.unbind(self.device_id)
        except:
            pass
        self.status = False

    def read_attr(self, attr):
        path = self.path / attr
        if not os.path.exists(path):
            logger.info(f"attr '{attr}' not exists")
            return None
        logger.debug(f"read from {path}")
        return path.read_text().rstrip()

    def write_attr(self, attr, val):
        path = self.path / attr
        if not os.path.exists(path):
            logger.info(f"attr '{attr}' not exists")
            return
        logger.debug(f"write '{val}' to {path}")
        return path.write_bytes(val.encode())
