#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging

from pathlib import Path

from ..ddunit import DriverTest
from ..device import Device
from ..mockup import Mockup

logger = logging.getLogger(__name__)

class SPIDriverTest(DriverTest):
    bus = 'spi'

    @property
    def bpf(self):
        return f'spi-xfer-r{self.regbytes}v{self.valbytes}'

class SPIDevice(Device):
    bus = 'spi'

    @property
    def device_id(self):
        return f"{self.bus}{self.busid}.{self.addr}"

SPI_MASTER_PATH = '/sys/class/spi_master/spi0'

class SPIMockup(Mockup):
    bus = 'spi'
    host = Path('/sys/kernel/config/spi-mockup/spi0')
    live = Path('/sys/kernel/config/spi-mockup/spi0/live')

    def setup(self):
        logger.debug('setup')
        if not self.host.exists():
            self.host.mkdir()

        self.live.write_text('true')

    def teardown(self):
        logger.debug('spi mockup teardown')
        self.live.write_text('false')
        self.host.rmdir()

    @property
    def device_id(self):
        return f"{self.devid} {self.addr}"

    def create_device(self):
        logger.debug(f'new device {self.devid} to spi bus')
        dev = Path(f'/sys/kernel/config/spi-mockup/spi0/targets/{self.devid}')
        if not dev.exists():
            dev.mkdir()
        device_id = Path(f'/sys/kernel/config/spi-mockup/spi0/targets/{self.devid}/device_id')
        device_id.write_text(self.devid)

        device_live = Path(f'/sys/kernel/config/spi-mockup/spi0/targets/{self.devid}/live')
        device_live.write_text('true')

    def remove_device(self):
        logger.debug(f'delete device {self.devid} from spi bus')
        logger.debug(f'delete device {self.devid} to spi bus')
        device_live = Path(f'/sys/kernel/config/spi-mockup/spi0/targets/{self.devid}/live')
        device_live.write_text('false')
        dev = Path(f'/sys/kernel/config/spi-mockup/spi0/targets/{self.devid}')
        dev.rmdir()

