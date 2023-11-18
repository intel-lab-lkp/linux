#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import re
import os
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

class FailModule(object):
    name = None
    key = None

    def __init__(self):
        self.has_support = os.path.exists(f"/sys/kernel/debug/{self.name}")
        self.ftext = Path(f"/sys/kernel/debug/{self.name}/require-start")
        self.fdata = Path(f"/sys/kernel/debug/{self.name}/require-end")
        self.fdepth = Path(f"/sys/kernel/debug/{self.name}/stacktrace-depth")
        self.nowarn = Path(f"/sys/kernel/debug/{self.name}/verbose")
        self.enabled = False
        self.module = None

    def feature_enabled(self):
        if not self.has_support:
            return False
        return self.enabled

    def filter_module(self, name):
        if name is None:
            self.module = None
        else:
            self.module = re.sub('-', '_', name)

    def enable_verbose(self):
        if not self.feature_enabled():
            return
        self.nowarn.write_text('1')

    def disable_verbose(self):
        if not self.feature_enabled():
            return
        self.nowarn.write_text('0')

    def enable_module_filter(self):
        if not self.feature_enabled():
            return
        if self.module is None:
            return
        logger.debug(f"enter module filter for fail {self.name}")
        mtext = Path(f"/sys/module/{self.module}/sections/.text")
        mdata = Path(f"/sys/module/{self.module}/sections/.data")
        self.ftext.write_text(mtext.read_text().rstrip())
        self.fdata.write_text(mdata.read_text().rstrip())
        self.fdepth.write_text('32')

    def disable_module_filter(self):
        if not self.feature_enabled():
            return
        if self.module is None:
            return
        logger.debug(f"exit module filter for fail {self.name}")
        self.ftext.write_text('0')
        self.fdata.write_text('0')
        self.fdepth.write_text('32')

    def enable_feature(self):
        pass

    def disable_feature(self):
        pass

    def start(self):
        self.enable_module_filter()
        self.enable_verbose()
        self.enable_feature()

    def stop(self):
        self.disable_feature()
        self.disable_verbose()
        self.disable_module_filter()
