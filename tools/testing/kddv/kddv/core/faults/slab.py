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

FAILSLAB_IGNORE = '/sys/kernel/debug/failslab/ignore-gfp-wait'

logger = logging.getLogger(__name__)

class FailSlab(FailModule):
    name = 'failslab'
    key = 'slab'

    def __init__(self):
        super().__init__()
        self.ignore = Path(FAILSLAB_IGNORE)

    def enable_feature(self):
        if not self.feature_enabled():
            return
        logger.debug("enter fail slab injection")
        self.ignore.write_text('N')

    def disable_feature(self):
        if not self.feature_enabled():
            return
        logger.debug("exit fail slab injection")
        self.ignore.write_text('Y')
