# SPDX-License-Identifier: GPL-2.0

import os
import shutil
import string
import subprocess
import random
import tempfile

from lib.py import cmd


class Remote:
    def __init__(self, name, dir_path):
        self.name = name
        self.dir_path = dir_path
        self._tmpdir = None
        self._ctl_dir = tempfile.mkdtemp(prefix="ssh-ksft-")
        self._ctl = os.path.join(self._ctl_dir, "ctl")
        self._ssh_opts = ["-o", "BatchMode=yes",
                          "-o", "ConnectTimeout=20",
                          "-o", "ControlMaster=auto",
                          "-o", f"ControlPath={self._ctl}",
                          "-o", "ControlPersist=30"]
        self._ssh_base = ["ssh", "-q"] + self._ssh_opts
        # Open the persistent connection
        self._master_started = False
        try:
            r = subprocess.run(self._ssh_base + ["-fNM", self.name],
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=30)
            if r.returncode:
                raise OSError(f"SSH master to {name} failed: {r.stderr.decode().strip()}")
            self._master_started = True
        finally:
            if not self._master_started:
                shutil.rmtree(self._ctl_dir, ignore_errors=True)

    def __del__(self):
        try:
            if self._tmpdir:
                cmd("rm -rf " + self._tmpdir, host=self)
                self._tmpdir = None
        except Exception:
            pass
        try:
            if self._master_started:
                self._master_started = False
                subprocess.run(self._ssh_base + ["-O", "exit", self.name],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=10)
        except Exception:
            pass
        shutil.rmtree(self._ctl_dir, ignore_errors=True)

    def cmd(self, comm, pty=False):
        args = []
        if pty:
            args += ["-tt"]
        return subprocess.Popen(self._ssh_base + args + [self.name, comm],
                                stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def _mktmp(self):
        return ''.join(random.choice(string.ascii_lowercase) for _ in range(8))

    def deploy(self, what):
        if not self._tmpdir:
            self._tmpdir = "/tmp/" + self._mktmp()
            cmd("mkdir " + self._tmpdir, host=self)
        file_name = self._tmpdir + "/" + self._mktmp() + os.path.basename(what)

        if not os.path.isabs(what):
            what = os.path.abspath(self.dir_path + "/" + what)

        cmd(["scp", "-q"] + self._ssh_opts +
             [what, f"{self.name}:{file_name}"])
        return file_name
