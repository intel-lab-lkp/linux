#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging

from .dmesg import KernelMessage
from .faulter import FaultInject
from .memleak import Kmemleak

logger = logging.getLogger(__name__)

class Environ(object):
    def __init__(self):
        self.kmsg = KernelMessage()
        self.leak = Kmemleak()
        self.fault = FaultInject()

    def setup(self):
        self.kmsg.setup()
        self.leak.setup()
        self.fault.setup()

    def teardown(self):
        self.fault.teardown()
        self.leak.teardown()
        self.kmsg.teardown()

    def enable_kmemleak(self):
        """Enable Kernel memory leak detector"""
        self.leak.enabled = True

    def check_failure(self):
        msg = self.kmsg.check_failure()
        if msg:
            return msg
        return self.leak.check_failure()

    def enable_fault_inject(self, feature):
        """Enable fault injection feature"""
        self.fault.enable_feature(feature)

    def fault_running(self):
        """Fault injection has been enabled"""
        return self.fault.running

    def enter_fault_inject(self):
        """Enter fault injection"""
        self.fault.start_features()

    def exit_fault_inject(self):
        """Exit fault injection"""
        return self.fault.stop_features()

    def notify_insmod(self, name):
        self.fault.filter_module(name)

    def notify_rmmod(self):
        self.fault.filter_module(None)

environ = Environ()
