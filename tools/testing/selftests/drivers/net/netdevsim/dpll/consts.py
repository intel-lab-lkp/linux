# SPDX-License-Identifier: GPL-2.0
#
# Constants useful in DPLL system integration testing.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

import os
from enum import Enum


KSRC = os.environ.get('KSRC', '')
YNLPATH = 'tools/net/ynl/'
YNLSPEC = 'Documentation/netlink/specs/dpll.yaml'


class DPLL_TYPE(Enum):
    PPS = 1
    EEC = 2


class DPLL_LOCK_STATUS(Enum):
    UNLOCKED = 1
    LOCKED = 2
    LOCKED_HO_ACK = 3
    HOLDOVER = 4


class DPLL_PIN_TYPE(Enum):
    MUX = 1
    EXT = 2
    SYNCE_ETH_PORT = 3
    INT_OSCILLATOR = 4
    GNSS = 5


class DPLL_PIN_CAPS(Enum):
    DIR_CAN_CHG = 1
    PRIO_CAN_CHG = 2
    STATE_CAN_CHG = 4
