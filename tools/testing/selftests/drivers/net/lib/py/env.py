# SPDX-License-Identifier: GPL-2.0

import os
import re
import time
from pathlib import Path
from lib.py import KsftSkipEx, KsftXfailEx
from lib.py import ksft_setup, wait_file
from lib.py import cmd, ethtool, ip, rand_port, CmdExitFailure
from lib.py import NetNS, NetdevSimDev
from lib.py import EthtoolFamily
from .remote import Remote


class NetDrvEnvBase:
    """
    Base class for a NIC / host environments

    Attributes:
      test_dir: Path to the source directory of the test
      net_lib_dir: Path to the net/lib directory
    """
    def __init__(self, src_path):
        self.src_path = Path(src_path)
        self.test_dir = self.src_path.parent.resolve()
        self.net_lib_dir = (Path(__file__).parent / "../../../../net/lib").resolve()

        self.env = self._load_env_file()

        # Following attrs must be set be inheriting classes
        self.dev = None

    def _load_env_file(self):
        env = os.environ.copy()

        src_dir = Path(self.src_path).parent.resolve()
        if not (src_dir / "net.config").exists():
            return ksft_setup(env)

        with open((src_dir / "net.config").as_posix(), 'r') as fp:
            for line in fp.readlines():
                full_file = line
                # Strip comments
                pos = line.find("#")
                if pos >= 0:
                    line = line[:pos]
                line = line.strip()
                if not line:
                    continue
                pair = line.split('=', maxsplit=1)
                if len(pair) != 2:
                    raise Exception("Can't parse configuration line:", full_file)
                env[pair[0]] = pair[1]
        return ksft_setup(env)

    def __del__(self):
        pass

    def __enter__(self):
        ip(f"link set dev {self.dev['ifname']} up")
        wait_file(f"/sys/class/net/{self.dev['ifname']}/carrier",
                  lambda x: x.strip() == "1")

        return self

    def __exit__(self, ex_type, ex_value, ex_tb):
        """
        __exit__ gets called at the end of a "with" block.
        """
        self.__del__()


class NetDrvEnv(NetDrvEnvBase):
    """
    Class for a single NIC / host env, with no remote end
    """
    def __init__(self, src_path, nsim_test=None, **kwargs):
        super().__init__(src_path)

        self._ns = None

        if 'NETIF' in self.env:
            if nsim_test is True:
                raise KsftXfailEx("Test only works on netdevsim")

            self.dev = ip("-d link show dev " + self.env['NETIF'], json=True)[0]
        else:
            if nsim_test is False:
                raise KsftXfailEx("Test does not work on netdevsim")

            self._ns = NetdevSimDev(**kwargs)
            self.dev = self._ns.nsims[0].dev
        self.ifname = self.dev['ifname']
        self.ifindex = self.dev['ifindex']

    def __del__(self):
        if self._ns:
            self._ns.remove()
            self._ns = None


class NetDrvEpEnv(NetDrvEnvBase):
    """
    Class for an environment with a local device and "remote endpoint"
    which can be used to send traffic in.

    For local testing it creates two network namespaces and a pair
    of netdevsim devices.
    """

    # Network prefixes used for local tests
    nsim_v4_pfx = "192.0.2."
    nsim_v6_pfx = "2001:db8::"

    def __init__(self, src_path, nsim_test=None):
        super().__init__(src_path)

        self._stats_settle_time = None

        # Things we try to destroy
        self.remote = None
        # These are for local testing state
        self._netns = None
        self._ns = None
        self._ns_peer = None

        self.addr_v        = { "4": None, "6": None }
        self.remote_addr_v = { "4": None, "6": None }

        if "NETIF" in self.env:
            if nsim_test is True:
                raise KsftXfailEx("Test only works on netdevsim")
            self._check_env()

            self.dev = ip("-d link show dev " + self.env['NETIF'], json=True)[0]

            self.addr_v["4"] = self.env.get("LOCAL_V4")
            self.addr_v["6"] = self.env.get("LOCAL_V6")
            self.remote_addr_v["4"] = self.env.get("REMOTE_V4")
            self.remote_addr_v["6"] = self.env.get("REMOTE_V6")
            kind = self.env["REMOTE_TYPE"]
            args = self.env["REMOTE_ARGS"]
        else:
            if nsim_test is False:
                raise KsftXfailEx("Test does not work on netdevsim")

            self.create_local()

            self.dev = self._ns.nsims[0].dev

            self.addr_v["4"] = self.nsim_v4_pfx + "1"
            self.addr_v["6"] = self.nsim_v6_pfx + "1"
            self.remote_addr_v["4"] = self.nsim_v4_pfx + "2"
            self.remote_addr_v["6"] = self.nsim_v6_pfx + "2"
            kind = "netns"
            args = self._netns.name

        self.remote = Remote(kind, args, src_path)

        self.addr_ipver = "6" if self.addr_v["6"] else "4"
        self.addr = self.addr_v[self.addr_ipver]
        self.remote_addr = self.remote_addr_v[self.addr_ipver]

        # Bracketed addresses, some commands need IPv6 to be inside []
        self.baddr = f"[{self.addr_v['6']}]" if self.addr_v["6"] else self.addr_v["4"]
        self.remote_baddr = f"[{self.remote_addr_v['6']}]" if self.remote_addr_v["6"] else self.remote_addr_v["4"]

        self.ifname = self.dev['ifname']
        self.ifindex = self.dev['ifindex']

        # resolve remote interface name
        self.remote_ifname = self.resolve_remote_ifc()

        self._required_cmd = {}

    def create_local(self):
        self._netns = NetNS()
        self._ns = NetdevSimDev()
        self._ns_peer = NetdevSimDev(ns=self._netns)

        with open("/proc/self/ns/net") as nsfd0, \
             open("/var/run/netns/" + self._netns.name) as nsfd1:
            ifi0 = self._ns.nsims[0].ifindex
            ifi1 = self._ns_peer.nsims[0].ifindex
            NetdevSimDev.ctrl_write('link_device',
                                    f'{nsfd0.fileno()}:{ifi0} {nsfd1.fileno()}:{ifi1}')

        ip(f"   addr add dev {self._ns.nsims[0].ifname} {self.nsim_v4_pfx}1/24")
        ip(f"-6 addr add dev {self._ns.nsims[0].ifname} {self.nsim_v6_pfx}1/64 nodad")
        ip(f"   link set dev {self._ns.nsims[0].ifname} up")

        ip(f"   addr add dev {self._ns_peer.nsims[0].ifname} {self.nsim_v4_pfx}2/24", ns=self._netns)
        ip(f"-6 addr add dev {self._ns_peer.nsims[0].ifname} {self.nsim_v6_pfx}2/64 nodad", ns=self._netns)
        ip(f"   link set dev {self._ns_peer.nsims[0].ifname} up", ns=self._netns)

    def _check_env(self):
        vars_needed = [
            ["LOCAL_V4", "LOCAL_V6"],
            ["LOCAL_PREFIX_V4", "LOCAL_PREFIX_V6"],
            ["REMOTE_V4", "REMOTE_V6"],
            ["REMOTE_TYPE"],
            ["REMOTE_ARGS"]
        ]
        missing = []

        for choice in vars_needed:
            for entry in choice:
                if entry in self.env:
                    break
            else:
                missing.append(choice)
        # Make sure v4 / v6 configs are symmetric
        if ("LOCAL_V6" in self.env) != ("REMOTE_V6" in self.env):
            missing.append(["LOCAL_V6", "REMOTE_V6"])
        if ("LOCAL_V4" in self.env) != ("REMOTE_V4" in self.env):
            missing.append(["LOCAL_V4", "REMOTE_V4"])
        if missing:
            raise Exception("Invalid environment, missing configuration:", missing,
                            "Please see tools/testing/selftests/drivers/net/README.rst")

    def resolve_remote_ifc(self):
        v4 = v6 = None
        if self.remote_addr_v["4"]:
            v4 = ip("addr show to " + self.remote_addr_v["4"], json=True, host=self.remote)
        if self.remote_addr_v["6"]:
            v6 = ip("addr show to " + self.remote_addr_v["6"], json=True, host=self.remote)
        if v4 and v6 and v4[0]["ifname"] != v6[0]["ifname"]:
            raise Exception("Can't resolve remote interface name, v4 and v6 don't match")
        if (v4 and len(v4) > 1) or (v6 and len(v6) > 1):
            raise Exception("Can't resolve remote interface name, multiple interfaces match")
        return v6[0]["ifname"] if v6 else v4[0]["ifname"]

    def __del__(self):
        if self._ns:
            self._ns.remove()
            self._ns = None
        if self._ns_peer:
            self._ns_peer.remove()
            self._ns_peer = None
        if self._netns:
            del self._netns
            self._netns = None
        if self.remote:
            del self.remote
            self.remote = None

    def require_ipver(self, ipver):
        if not self.addr_v[ipver] or not self.remote_addr_v[ipver]:
            raise KsftSkipEx(f"Test requires IPv{ipver} connectivity")

    def require_nsim(self):
        if self._ns is None:
            raise KsftXfailEx("Test only works on netdevsim")

    def _require_cmd(self, comm, key, host=None):
        cached = self._required_cmd.get(comm, {})
        if cached.get(key) is None:
            cached[key] = cmd("command -v -- " + comm, fail=False,
                              shell=True, host=host).ret == 0
        self._required_cmd[comm] = cached
        return cached[key]

    def require_cmd(self, comm, local=True, remote=False):
        if local:
            if not self._require_cmd(comm, "local"):
                raise KsftSkipEx("Test requires command: " + comm)
        if remote:
            if not self._require_cmd(comm, "remote", host=self.remote):
                raise KsftSkipEx("Test requires (remote) command: " + comm)

    def wait_hw_stats_settle(self):
        """
        Wait for HW stats to become consistent, some devices DMA HW stats
        periodically so events won't be reflected until next sync.
        Good drivers will tell us via ethtool what their sync period is.
        """
        if self._stats_settle_time is None:
            data = {}
            try:
                data = ethtool("-c " + self.ifname, json=True)[0]
            except CmdExitFailure as e:
                if "Operation not supported" not in e.cmd.stderr:
                    raise

            self._stats_settle_time = 0.025 + \
                data.get('stats-block-usecs', 0) / 1000 / 1000

        time.sleep(self._stats_settle_time)


class MemPrvEnv(NetDrvEpEnv):
    def __init__(self, src_path, rss=False, rss_num=1, **kwargs):
        super().__init__(src_path, False, **kwargs)

        self.ethnl = EthtoolFamily()
        self.cleaned_up = False

        channels = self.ethnl.channels_get({'header': {'dev-index': self.ifindex}})
        self.channels = channels['combined-count']
        if self.channels < 2:
            raise KsftSkipEx('Test requires NETIF with at least 2 combined channels')

        if rss and rss_num > self.channels - 1:
            raise KsftSkipEx(f"Test with {rss_num} queues in RSS context requires NETIF with at least {rss_num + 1} combined channels")

        self.port = rand_port()
        rings = self.ethnl.rings_get({'header': {'dev-index': self.ifindex}})
        self.rx_rings = rings['rx']
        self.hds_thresh = rings.get('hds-thresh', 0)
        self.ethnl.rings_set({'header': {'dev-index': self.ifindex},
                              'tcp-data-split': 'enabled',
                              'hds-thresh': 0,
                              'rx': 64})

        if rss:
            self.target_queue = self.channels - rss_num
            ethtool(f"-X {self.ifname} equal {self.target_queue}")
            self.rss_ctx_id = self._create_rss_ctx(rss_num)
            self.rule_id = self._set_rss_flow_rule()
        else:
            self.target_queue = self.channels - 1
            ethtool(f"-X {self.ifname} equal {self.target_queue}")
            self.rss_ctx_id = None
            self.rule_id = self._set_flow_rule()

    def __del__(self):
        if self.cleaned_up:
            return

        ethtool(f"-N {self.ifname} delete {self.rule_id}")
        if self.rss_ctx_id:
            self.ethnl.rss_delete_act({'header': {'dev-index': self.ifindex},
                                       'context': self.rss_ctx_id})

        ethtool(f"-X {self.ifname} default")
        self.ethnl.rings_set({'header': {'dev-index': self.ifindex},
                              'tcp-data-split': 'unknown',
                              'hds-thresh': self.hds_thresh,
                              'rx': self.rx_rings})
        self.cleaned_up = True

    def _set_flow_rule(self):
        output = ethtool(f"-N {self.ifname} flow-type tcp6 dst-port {self.port} action {self.target_queue}").stdout
        values = re.search(r'ID (\d+)', output).group(1)
        return int(values)

    def _set_rss_flow_rule(self):
        output = ethtool(f"-N {self.ifname} flow-type tcp6 dst-port {self.port} context {self.rss_ctx_id}").stdout
        values = re.search(r'ID (\d+)', output).group(1)
        return int(values)

    def _create_rss_ctx(self, num):
        output = ethtool(f"-X {self.ifname} context new start {self.target_queue} equal {num}").stdout
        values = re.search(r'New RSS context is (\d+)', output).group(1)
        return int(values)
