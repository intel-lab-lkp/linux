#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging

from .dmesg import KernelMessage
from .memleak import Kmemleak

logger = logging.getLogger(__name__)

class Environ(object):
    def __init__(self):
        self.kmsg = KernelMessage()
        self.leak = Kmemleak()

    def setup(self):
        self.kmsg.setup()
        self.leak.setup()

    def teardown(self):
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

environ = Environ()
