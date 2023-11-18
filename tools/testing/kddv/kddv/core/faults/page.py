#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging
from pathlib import Path

from .fail import FailModule

FAILPAGE_IGNORE_HMEM = '/sys/kernel/debug/fail_page_alloc/ignore-gfp-highmem'
FAILPAGE_IGNORE_WAIT = '/sys/kernel/debug/fail_page_alloc/ignore-gfp-wait'

logger = logging.getLogger(__name__)

class FailPage(FailModule):
    name = 'fail_page_alloc'
    key = 'page'

    def __init__(self):
        super().__init__()
        self.ignore_hmem = Path(FAILPAGE_IGNORE_HMEM)
        self.ignore_wait = Path(FAILPAGE_IGNORE_WAIT)

    def enable_feature(self):
        if not self.feature_enabled():
            return
        logger.debug("enter fail page injection")
        self.ignore_hmem.write_text('N')
        self.ignore_wait.write_text('N')

    def disable_feature(self):
        if not self.feature_enabled():
            return
        logger.debug("exit fail page injection")
        self.ignore_hmem.write_text('Y')
        self.ignore_wait.write_text('Y')
