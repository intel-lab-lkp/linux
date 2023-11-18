#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

__all__ = ['FailModule']

from .fail import FailModule
from .slab import FailSlab
from .page import FailPage
