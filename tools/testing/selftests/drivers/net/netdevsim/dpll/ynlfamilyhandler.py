# SPDX-License-Identifier: GPL-2.0
#
# Wrapper for the YNL library used to interact with the netlink interface.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

import sys
from pathlib import Path
from dataclasses import dataclass

from .consts import KSRC, YNLSPEC, YNLPATH


try:
    ynl_full_path = Path(KSRC) / YNLPATH
    sys.path.append(ynl_full_path.as_posix())
    from lib import YnlFamily
except ModuleNotFoundError:
    print("Failed importing `ynl` library from kernel sources, please set KSRC")
    sys.exit(1)


@dataclass
class YnlFamilyHandler:
    spec: str = Path(KSRC) / YNLSPEC
    schema: str = ''
    dump: str = ''
    ntf: str = ''
    do: str = ''
    attrs = {}

    def __post_init__(self):
        self.ynl = YnlFamily(self.spec, self.schema)

        if self.ntf:
            self.ynl.ntf_subscribe(self.ntf)

    def execute(self):
        if self.do and self.dump:
            raise ValueError('Both "do" or "dump" set simultaneously - clear either of them')
        elif self.do:
            reply = self.ynl.do(self.do, self.attrs, [])
        elif self.dump:
            reply = self.ynl.dump(self.dump, self.attrs)
        else:
            raise ValueError('Wrong command - Set either "do" or "dump" before executing')

        return reply
