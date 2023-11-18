#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import re
import logging
import subprocess

KERNEL_PANIC = [
    "BUG:",
    "WARNING:",
    "INFO:",
    "[kK]ernel BUG",
    "PANIC: double fault",
    "divide error:",
    "UBSAN:",
    "Unable to handle kernel",
    "general protection fault",
]

logger = logging.getLogger(__name__)

class KernelMessage(object):

    def setup(self):
        subprocess.run(["/usr/bin/dmesg", "-C"])

    def teardown(self):
        subprocess.run(["/usr/bin/dmesg", "-C"])

    def check_failure(self):
        logger.debug('check kernel message')
        kmsg = subprocess.check_output(["/usr/bin/dmesg", "-c"])
        regex_pattern = re.compile("|".join(KERNEL_PANIC))
        if regex_pattern.search(kmsg.decode()):
            return kmsg.decode()
        return None
