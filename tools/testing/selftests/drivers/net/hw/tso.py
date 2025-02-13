#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Run the tools/testing/selftests/net/csum testsuite."""

import fcntl
import socket
import struct
import termios
import time

from lib.py import ksft_pr, ksft_run, ksft_exit, KsftSkipEx, KsftXfailEx
from lib.py import ksft_eq, ksft_ge, ksft_lt
from lib.py import EthtoolFamily, NetdevFamily, NetDrvEpEnv
from lib.py import bkg, cmd, defer, ethtool, ip, rand_port, wait_port_listen


def sock_wait_drain(sock, max_wait=1000):
    """Wait for all pending write data on the socket to get ACKed."""
    for _ in range(max_wait):
        one = b'\0' * 4
        outq = fcntl.ioctl(sock.fileno(), termios.TIOCOUTQ, one)
        outq = struct.unpack("I", outq)[0]
        if outq == 0:
            break
        time.sleep(0.01)
    ksft_eq(outq, 0)


def tcp_sock_get_retrans(sock):
    """Get the number of retransmissions for the TCP socket."""
    info = sock.getsockopt(socket.SOL_TCP, socket.TCP_INFO, 512)
    return struct.unpack("I", info[100:104])[0]


def run_one_stream(cfg, ipv4, remote_v4, remote_v6, should_lso):
    cfg.require_cmd("socat", remote=True)

    port = rand_port()
    listen_cmd = f"socat -{cfg.addr_ipver} -t 2 -u TCP-LISTEN:{port},reuseport /dev/null,ignoreeof"

    with bkg(listen_cmd, host=cfg.remote) as nc:
        wait_port_listen(port, host=cfg.remote)

        if ipv4:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((remote_v4, port))
        else:
            sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            sock.connect((remote_v6, port))

        # Small send to make sure the connection is working.
        sock.send("ping".encode())
        sock_wait_drain(sock)

        # Send 4MB of data, record the LSO packet count.
        qstat_old = cfg.netnl.qstats_get({"ifindex": cfg.ifindex}, dump=True)[0]
        buf = b"0" * 1024 * 1024 * 4
        sock.send(buf)
        sock_wait_drain(sock)
        qstat_new = cfg.netnl.qstats_get({"ifindex": cfg.ifindex}, dump=True)[0]

        # No math behind the 10 here, but try to catch cases where
        # TCP falls back to non-LSO.
        ksft_lt(tcp_sock_get_retrans(sock), 10)
        sock.close()

        # Check that at least 90% of the data was sent as LSO packets.
        # System noise may cause false negatives, it is what it is.
        total_lso_wire  = len(buf) * 0.90 // cfg.dev["mtu"]
        total_lso_super = len(buf) * 0.90 // cfg.dev["tso_max_size"]
        if should_lso:
            if cfg.have_stat_super_count:
                ksft_ge(qstat_new['tx-hw-gso-packets'] -
                        qstat_old['tx-hw-gso-packets'],
                        total_lso_super,
                        comment="Number of LSO super-packets with LSO enabled")
            if cfg.have_stat_wire_count:
                ksft_ge(qstat_new['tx-hw-gso-wire-packets'] -
                        qstat_old['tx-hw-gso-wire-packets'],
                        total_lso_wire,
                        comment="Number of LSO wire-packets with LSO enabled")
        else:
            if cfg.have_stat_super_count:
                ksft_lt(qstat_new['tx-hw-gso-packets'] -
                        qstat_old['tx-hw-gso-packets'],
                        100, comment="Number of LSO super-packets with LSO disabled")
            if cfg.have_stat_wire_count:
                ksft_lt(qstat_new['tx-hw-gso-wire-packets'] -
                        qstat_old['tx-hw-gso-wire-packets'],
                        1000, comment="Number of LSO wire-packets with LSO disabled")


def build_tunnel(cfg, outer_ipv4, tun_info):
    local_v4  = NetDrvEpEnv.nsim_v4_pfx + "1"
    local_v6  = NetDrvEpEnv.nsim_v6_pfx + "1"
    remote_v4 = NetDrvEpEnv.nsim_v4_pfx + "2"
    remote_v6 = NetDrvEpEnv.nsim_v6_pfx + "2"

    if outer_ipv4:
        local_addr  = cfg.v4
        remote_addr = cfg.remote_v4
    else:
        local_addr  = cfg.v6
        remote_addr = cfg.remote_v6

    tun_type = tun_info[0]
    tun_arg  = tun_info[1]
    ip(f"link add {tun_type}-ksft type {tun_type} {tun_arg} local {local_addr} remote {remote_addr} dev {cfg.ifname}")
    defer(ip, f"link del {tun_type}-ksft")
    ip(f"link set dev {tun_type}-ksft up")
    ip(f"addr add {local_v4}/24 dev {tun_type}-ksft")
    ip(f"addr add {local_v6}/64 dev {tun_type}-ksft")

    ip(f"link add {tun_type}-ksft type {tun_type} {tun_arg} local {remote_addr} remote {local_addr} dev {cfg.remote_ifname}",
        host=cfg.remote)
    defer(ip, f"link del {tun_type}-ksft", host=cfg.remote)
    ip(f"link set dev {tun_type}-ksft up", host=cfg.remote)
    ip(f"addr add {remote_v4}/24 dev {tun_type}-ksft", host=cfg.remote)
    ip(f"addr add {remote_v6}/64 dev {tun_type}-ksft", host=cfg.remote)

    return remote_v4, remote_v6


def test_builder(name, cfg, ipv4, feature, tun=None, inner_ipv4=None):
    """Construct specific tests from the common template."""
    def f(cfg):
        if ipv4:
            cfg.require_v4()
        else:
            cfg.require_v6()

        if not cfg.have_stat_super_count and \
           not cfg.have_stat_wire_count:
            raise KsftSkipEx(f"Device does not support LSO queue stats")

        if tun:
            remote_v4, remote_v6 = build_tunnel(cfg, ipv4, tun)
        else:
            remote_v4 = cfg.remote_v4
            remote_v6 = cfg.remote_v6

        has_gso_partial = tun and 'tx-gso-partial' in cfg.features

        # First test without the feature enabled.
        ethtool(f"-K {cfg.ifname} {feature} off")
        if has_gso_partial:
            ethtool(f"-K {cfg.ifname} tx-gso-partial off")
        run_one_stream(cfg, ipv4, remote_v4, remote_v6, should_lso=False)

        # Now test with the feature enabled.
        if has_gso_partial:
            ethtool(f"-K {cfg.ifname} tx-gso-partial on")
        if feature in cfg.features:
            ethtool(f"-K {cfg.ifname} {feature} on")
            run_one_stream(cfg, ipv4, remote_v4, remote_v6, should_lso=True)
        else:
            raise KsftXfailEx(f"Device does not support {feature}")

    if tun:
        name += ("4" if inner_ipv4 else "6") + "_"
    if ipv4:
        f.__name__ = name + "ipv4"
    else:
        f.__name__ = name + "ipv6"
    return f


def query_nic_features(cfg) -> None:
    """Query and cache the NIC features."""
    cfg.features = set()

    cfg.have_stat_super_count = False
    cfg.have_stat_wire_count = False

    features = cfg.ethnl.features_get({"header": {"dev-index": cfg.ifindex}})
    for f in features["active"]["bits"]["bit"]:
        cfg.features.add(f["name"])
    for f in features["hw"]["bits"]["bit"]:
        cfg.features.add(f["name"])

    stats = cfg.netnl.qstats_get({"ifindex": cfg.ifindex}, dump=True)
    if stats:
        if 'tx-hw-gso-packets' in stats[0]:
            ksft_pr("Detected qstat for LSO super-packets")
            cfg.have_stat_super_count = True
        if 'tx-hw-gso-wire-packets' in stats[0]:
            ksft_pr("Detected qstat for LSO wire-packets")
            cfg.have_stat_wire_count = True


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        cfg.netnl = NetdevFamily()

        query_nic_features(cfg)

        tun_info = (
            # name,         ethtool_feature              tun:(type,    args   4/6 only)
            ("",            "tx-tcp6-segmentation",          None),
            ("vxlan",       "tx-udp_tnl-segmentation",       ("vxlan", "id 100 dstport 4789 noudpcsum")),
            ("vxlan_csum",  "tx-udp_tnl-csum-segmentation",  ("vxlan", "id 100 dstport 4789 udpcsum")),
            ("gre",         "tx-udp_tnl-segmentation",       ("ipgre",  "",   True)),
            ("gre",         "tx-udp_tnl-segmentation",       ("ip6gre", "",   False)),
        )

        cases = []
        for outer_ipv4 in [True, False]:
            for info in tun_info:
                # Skip if it's tunnel which only works for a specific IP version
                if info[2] and len(info[2]) > 2 and outer_ipv4 != info[2][2]:
                    continue

                cases.append(test_builder(info[0], cfg, outer_ipv4, info[1],
                                          tun=info[2], inner_ipv4=True))
                if info[2]:
                    cases.append(test_builder(info[0], cfg, outer_ipv4, info[1],
                                              tun=info[2], inner_ipv4=False))

        ksft_run(cases=cases, args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
