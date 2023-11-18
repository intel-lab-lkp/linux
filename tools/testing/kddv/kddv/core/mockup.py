#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import os
import re
import json
import struct
import logging
import subprocess

from pathlib import Path

logger = logging.getLogger(__name__)

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT_BPF = os.path.join(ROOT_DIR, 'data', 'bpf')
DEST_BPF = '/sys/fs/bpf'

class Mockup(object):
    bus = None

    def __init__(self, bus, p) -> None:
        self.bus = bus
        self.bpf = p.bpf
        self.addr = p.address
        self.devid = p.device_id
        self.configs = p.configs
        self.regshift = p.regshift
        self.regbytes = p.regbytes
        self.valbytes = p.valbytes
        self.regmaps = p.regmaps

    @classmethod
    def create(cls, bus, p):
        for sub in cls.__subclasses__():
            if sub.bus == bus:
                return sub(bus, p)
        return cls(bus, p)

    @property
    def device_id(self):
        return f"{self.addr}"

    @property
    def bpftool(self):
        return os.environ.get('BPFTOOL_PATH', 'bpftool')

    def get_valbytes(self):
        if self.valbytes == 3:
            return 4
        return self.valbytes

    def search_file(self, path, filename):
        for curfile in os.listdir(path):
            abspath = os.path.join(path, curfile)
            if os.path.isdir(abspath):
                subfile = self.search_file(abspath, filename)
                if subfile is not None:
                    return subfile
            if curfile == filename:
                return abspath
        return None

    def bpf_prog_name(self):
        bpf_name = re.sub("-", "_", os.path.basename(self.bpf))
        return f'{bpf_name}'[:15]

    def load_bpf(self):
        if self.bpf is None:
            return
        bpf_file = self.search_file(ROOT_BPF, f"{self.bpf}.o")
        if bpf_file is None:
            logger.error(f'bpf file {self.bpf} not found')
            return
        logger.debug(f'load bpf {self.bpf}.o')
        bpf_path = os.path.join(DEST_BPF, self.bpf_prog_name())
        if os.path.exists(bpf_path):
            os.unlink(bpf_path)
        cmds = [self.bpftool, 'prog', 'load']
        cmds += [bpf_file, bpf_path]
        cmds += ['autoattach']
        logger.debug(' '.join(cmds))
        subprocess.check_output(cmds)

    def unload_bpf(self):
        if self.bpf is None:
            return
        logger.debug(f'unload bpf {self.bpf}.o')
        bpf_path = os.path.join(DEST_BPF, self.bpf_prog_name())
        if os.path.exists(bpf_path):
            os.unlink(bpf_path)

    def create_device(self):
        pass

    def remove_device(self):
        pass

    def load(self):
        self.load_bpf()
        self.load_regmaps()
        self.load_configs()
        self.create_device()

    def unload(self):
        self.remove_device()
        self.unload_bpf()

    def bpf_map_name(self):
        bpf_name = re.sub("-", "_", self.bpf)
        return f'regs_{bpf_name}'[:15]

    def to_bpf_bytes(self, val, len):
        return list("%d" % n for n in list(val.to_bytes(len, 'little')))

    def write_bpf_map(self, name, addr, val):
        cmds = [self.bpftool, 'map', 'update']
        cmds += ['name', name]
        cmds += ['key']
        cmds += self.to_bpf_bytes(addr, 4)
        cmds += ['value']
        cmds += self.to_bpf_bytes(val, 4)
        logger.debug(' '.join(cmds))
        subprocess.check_output(cmds)

    def write_config(self, addr, val):
        if self.bpf is None:
            return
        self.write_bpf_map('bpf_xfer_conf', addr, val)

    def write_configs(self, addr, data):
        for i in range(len(data)):
            self.write_config(addr + i, data[i])

    def load_configs(self):
        for reg, value in self.configs.items():
            if isinstance(value, list):
                self.write_configs(reg, value)
            else:
                self.write_config(reg, value)

    def load_regmaps(self):
        for reg, value in self.regmaps.items():
            if isinstance(value, list):
                self.write_regs(reg, value)
            else:
                self.write_reg(reg, value)

    def read_reg(self, addr):
        if self.bpf is None:
            return
        cmds = [self.bpftool, 'map', 'lookup']
        cmds += ['name', self.bpf_map_name()]
        cmds += ['key']
        cmds += self.to_bpf_bytes(addr, 4)
        logger.debug(' '.join(cmds))
        mapval = subprocess.check_output(cmds)
        return json.loads(mapval).get("value", 0)

    def read_regs(self, addr, len):
        data = []
        for i in range(len):
            data += [self.read_reg(addr + (i << self.regshift))]
        return data

    def write_reg(self, addr, val):
        if self.bpf is None:
            return
        cmds = [self.bpftool, 'map', 'update']
        cmds += ['name', self.bpf_map_name()]
        cmds += ['key']
        cmds += self.to_bpf_bytes(addr, 4)
        cmds += ['value']
        cmds += self.to_bpf_bytes(val, self.get_valbytes())
        logger.debug(' '.join(cmds))
        subprocess.check_output(cmds)

    def write_regs(self, addr, data):
        for i in range(len(data)):
            self.write_reg(addr + (i << self.regshift), data[i])
