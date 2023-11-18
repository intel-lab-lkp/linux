#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import sys
import logging

logger = logging.getLogger()

LOG_FORMAT = "%(asctime)-15s [%(levelname)-7s] %(message)s"
LOG_LEVELS = {
    'ERROR': logging.ERROR,
    'WARN': logging.WARN,
    'INFO': logging.INFO,
    'DEBUG': logging.DEBUG
}

def setup_logger(level):
    logger.setLevel(LOG_LEVELS.get(level))
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter(
        fmt=LOG_FORMAT, datefmt="%Y-%m-%d %H:%M:%S"
    ))
    logger.addHandler(handler)
