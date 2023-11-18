#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import logging
import pathlib

KMEMLEAK = '/sys/kernel/debug/kmemleak'

logger = logging.getLogger(__name__)

class Kmemleak(object):
    def __init__(self):
        self.has_feature = os.path.exists(KMEMLEAK)
        self.ctrl = pathlib.Path(KMEMLEAK)
        self.enabled = False

    def setup(self):
        if not self.has_feature or not self.enabled:
            return
        self.ctrl.write_text('clear')

    def teardown(self):
        if not self.has_feature or not self.enabled:
            return
        self.ctrl.write_text('clear')

    def check_failure(self):
        if not self.has_feature or not self.enabled:
            return None
        logger.debug('check kernel memleak')
        self.ctrl.write_text('scan')
        self.ctrl.write_text('scan')
        return self.ctrl.read_text().rstrip()
