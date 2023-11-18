#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging

from .dmesg import KernelMessage

logger = logging.getLogger(__name__)

class Environ(object):
    def __init__(self):
        self.kmsg = KernelMessage()

    def setup(self):
        self.kmsg.setup()

    def teardown(self):
        self.kmsg.teardown()

    def check_failure(self):
        return self.kmsg.check_failure()

environ = Environ()
