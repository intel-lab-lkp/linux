#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Test possible UDP length overflow in udp_send_skb/udp_v6_send_skb.

import errno
import gzip
import os
import socket
import struct
import subprocess
from contextlib import contextmanager

from lib.py import KsftSkipEx, ksft_eq, ksft_raises, ksft_true
from lib.py import KsftNamedVariant, ksft_exit, ksft_pr, ksft_run, ksft_variants
from lib.py import NetNS, NetNSEnter, ip


IP_MTU_DISCOVER = 10
IP_PMTUDISC_PROBE = 3
IPV6_MTU_DISCOVER = 23
IPV6_PMTUDISC_DO = 2
IPV6_PMTUDISC_PROBE = 3
IPV6_TLV_JUMBO = 194


def check_kernel_config(option: str) -> bool | None:
    for filename, method in [
        ('/proc/config.gz', gzip.open),
        (f'/boot/config-{os.uname().release}', open),
    ]:
        try:
            with method(filename, 'rt') as config:
                for line in config:
                    if line.rstrip() == f'{option}=y':
                        return True
                return False
        except OSError:
            continue
        return None


def assert_debug_kernel() -> None:
    res = check_kernel_config('CONFIG_DEBUG_NET')
    if res is None:
        ksft_pr("WARN: Can't read kernel config; assuming debug kernel, and running the test")
    elif not res:
        raise KsftSkipEx('CONFIG_DEBUG_NET is not set')


def check_dmesg_clean(func: str) -> bool:
    with subprocess.Popen(['dmesg'], stdout=subprocess.PIPE) as dmesg:
        res = subprocess.run(['grep', '-q', f'WARNING:.*{func}'], stdin=dmesg.stdout, check=False)
    return res.returncode != 0 and dmesg.returncode == 0


@contextmanager
def dummy_netdev(ns: NetNS, mtu: int, ipv6: bool) -> None:
    try:
        ip('link add dummy type dummy', ns=ns)
        ip(f'link set dummy mtu {mtu}', ns=ns)
        ip('link set dummy up', ns=ns)
        flag = '-6' if ipv6 else ''
        nodad = 'nodad' if ipv6 else ''
        local = 'fd00::1/64' if ipv6 else '10.0.0.1/24'
        remote = 'fd00::2' if ipv6 else '10.0.0.2'
        ip(f'{flag} addr add {local} dev dummy {nodad}', ns=ns)
        ip(f'{flag} neigh add {remote} lladdr 02:00:00:00:00:02 dev dummy nud permanent', ns=ns)
        yield
    finally:
        ip('link del dummy', ns=ns)


@ksft_variants([
    KsftNamedVariant(
        'ipv6',
        True,
        socket.AF_INET6,
        (socket.IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_DO),
        'fd00::2',
        'udp_v6_send_skb',
    ),
    KsftNamedVariant(
        'ipv4',
        False,
        socket.AF_INET,
        (socket.IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_PROBE),
        '10.0.0.2',
        'udp_send_skb',
    ),
])
def test_udp(
    ipv6: bool,
    af: socket.AddressFamily,
    sockopts: tuple[int, int, int],
    destip: str,
    func: str
) -> None:
    if not ipv6:
        assert_debug_kernel()

    with (
        NetNS() as ns,
        dummy_netdev(ns, 65556 + 20 * ipv6, ipv6),
        NetNSEnter(ns),
        socket.socket(af, socket.SOCK_DGRAM) as fd
    ):
        fd.setsockopt(*sockopts)
        with ksft_raises(OSError) as e:
            fd.sendto(b' ' * 65528, (destip, 1234))
        # IPv6: EMSGSIZE happens on kernels with the fix.
        # IPv4: EMSGSIZE happens on both fixed and unfixed kernels, after the
        #       WARN is printed - ignore it and rely on the dmesg check.
        if e.exception is not None:
            ksft_eq(e.exception.errno, errno.EMSGSIZE)

    ksft_true(check_dmesg_clean(func), 'WARNING detected in dmesg')


def test_ipv6_jumbo() -> None:
    with (
        NetNS() as ns,
        dummy_netdev(ns, 65584, True),
        NetNSEnter(ns),
        socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_UDP) as fd
    ):
        hopopts = struct.pack('!BBBBI', 0, 0, IPV6_TLV_JUMBO, 4, 65544)
        fd.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_HOPOPTS, hopopts)
        fd.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 6)
        fd.setsockopt(socket.IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_PROBE)
        udp = struct.pack('!HHHH', 1234, 1234, 0, 0) + b' ' * 65528
        fd.sendto(udp, ('fd00::2', 0))


if __name__ == "__main__":
    ksft_run([
        test_udp,
        test_ipv6_jumbo,
    ])
    ksft_exit()
