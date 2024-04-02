# SPDX-License-Identifier: GPL-2.0

import os
import shlex
from pathlib import Path
from lib.py import ip
from lib.py import NetdevSimDev

class NetDrvEnv:
    def __init__(self, src_path):
        self.env = os.environ.copy()
        self._load_env_file(src_path)

        if 'NETIF' in self.env:
            self._ns = None
            self.dev = ip("link show dev " + self.env['NETIF'], json=True)[0]
        else:
            self._ns = NetdevSimDev()
            self.dev = self._ns.nsims[0].dev
        self.ifindex = self.dev['ifindex']

    def __del__(self):
        if self._ns:
            self._ns.remove()

    def _load_env_file(self, src_path):
        src_dir = Path(src_path).parent.resolve()
        if not (src_dir / "net.config").exists():
            return

        lexer = shlex.shlex(open((src_dir / "net.config").as_posix(), 'r').read())
        k = None
        for token in lexer:
            if k is None:
                k = token
                self.env[k] = ""
            elif token == "=":
                pass
            else:
                self.env[k] = token
                k = None
