#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import re
import logging

from pathlib import Path
from .environ import environ

logger = logging.getLogger(__name__)

class FaultIterator(object):
    def __init__(self, max_loop = 0):
        self._max_loop = max_loop
        self._cur_fail = 0
        self._max_fail = 3
        self.path = Path(f"/proc/self/fail-nth")

    def __iter__(self):
        self.nth = -1
        return self

    def __next__(self):
        self.nth += 1
        if not self.nth:
            return self.nth
        if not environ.fault_running():
            logger.debug('fault inject not running')
            raise StopIteration
        if not os.path.exists(self.path):
            logger.debug('fault inject not exists')
            raise StopIteration
        if self._max_loop and self._max_loop < self.nth:
            raise StopIteration
        if self.read_nth() > 0:
            self.write_nth(0)
            self._cur_fail += 1
            if self._cur_fail >= self._max_fail:
                logger.debug('end fault inject')
                raise StopIteration
        else:
           self._cur_fail = 0
        self.write_nth(self.nth)
        return self.nth

    def read_nth(self):
        return int(self.path.read_text().rstrip())

    def write_nth(self, val):
        logger.debug(f"write {val} to fail-nth")
        self.path.write_text(str(val))
