#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

from .faults import FailModule

class FaultInject(object):
    def __init__(self):
        self.enabled = False
        self.running = False
        self.features = []
        for subclass in FailModule.__subclasses__():
            self.features.append(subclass())

    def setup(self):
        pass

    def teardown(self):
        self.running = False

    def start_features(self):
        if not self.enabled:
            return
        for feature in self.features:
            feature.start()
        self.running = True

    def stop_features(self):
        if not self.enabled:
            return False
        for feature in self.features:
            feature.stop()
        return True

    def filter_module(self, module):
        for feature in self.features:
            feature.filter_module(module)

    def enable_feature(self, name):
        for feature in self.features:
            if name in [feature.key, 'all']:
                if feature.has_support:
                    feature.enabled = True
                    self.enabled = True
