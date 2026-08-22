#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Test possible UDP length overflow in udp_send_skb/udp_v6_send_skb.

from lib.py import ksft_run, ksft_exit, ksft_true, KsftSkipEx
from lib.py import ip, NetNS, NetNSEnter
import errno
import gzip
import os
import socket
import struct
import subprocess


IP_MTU_DISCOVER = 10
IP_PMTUDISC_PROBE = 3
IPV6_MTU_DISCOVER = 23
IPV6_PMTUDISC_DO = 2
IPV6_PMTUDISC_PROBE = 3
IPV6_TLV_JUMBO = 194


def check_kernel_config(option) -> bool | None:
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


def assert_debug_kernel() -> None:
    res = check_kernel_config('CONFIG_DEBUG_NET')
    if res is None:
        print("WARN: Can't read kernel config; assuming debug kernel, and running the test")
    elif not res:
        raise KsftSkipEx('CONFIG_DEBUG_NET is not set')


def check_dmesg_clean(func) -> bool:
    dmesg = subprocess.Popen(['dmesg'], stdout=subprocess.PIPE)
    result = subprocess.run(['grep', '-q', f'WARNING:.*{func}'], stdin=dmesg.stdout)
    dmesg.wait()
    return result.returncode != 0 and dmesg.returncode == 0


def ip_setup(ns: NetNS, mtu: int, ipv6: bool) -> None:
    ip('link add dummy type dummy', ns=ns)
    ip(f'link set dummy mtu {mtu}', ns=ns)
    ip('link set dummy up', ns=ns)
    flag = '-6' if ipv6 else ''
    nodad = 'nodad' if ipv6 else ''
    addr_local = 'fd00::1/64' if ipv6 else '10.0.0.1/24'
    addr_remote = 'fd00::2' if ipv6 else '10.0.0.2'
    ip(f'{flag} addr add {addr_local} dev dummy {nodad}', ns=ns)
    ip(f'{flag} neigh add {addr_remote} lladdr 02:00:00:00:00:02 dev dummy nud permanent', ns=ns)


def test_ipv6() -> None:
    assert_debug_kernel()

    with NetNS() as ns:
        ip_setup(ns, 65576, True)

        with NetNSEnter(ns):
            with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as fd:
                fd.setsockopt(socket.IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_DO)
                try:
                    fd.sendto(b' ' * 65528, ('fd00::2', 1234))
                except OSError as e:
                    # Ignore EMSGSIZE: it happens on kernels with the fix.
                    if e.errno != errno.EMSGSIZE:
                        raise

        ip('link del dummy', ns=ns)

    ksft_true(check_dmesg_clean('udp_v6_send_skb'), 'WARNING detected in dmesg')


def test_ipv4() -> None:
    assert_debug_kernel()

    with NetNS() as ns:
        ip_setup(ns, 65556, False)

        with NetNSEnter(ns):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as fd:
                fd.setsockopt(socket.IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_PROBE)
                try:
                    fd.sendto(b' ' * 65528, ('10.0.0.2', 1234))
                except OSError as e:
                    # Ignore EMSGSIZE: the check happens after the WARN is printed.
                    if e.errno != errno.EMSGSIZE:
                        raise

        ip('link del dummy', ns=ns)

    ksft_true(check_dmesg_clean('udp_send_skb'), 'WARNING detected in dmesg')


def test_ipv6_jumbo() -> None:
    with NetNS() as ns:
        ip_setup(ns, 65584, True)

        with NetNSEnter(ns):
            with socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_UDP) as fd:
                hopopts = struct.pack('!BBBBI', 0, 0, IPV6_TLV_JUMBO, 4, 65544)
                fd.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_HOPOPTS, hopopts)
                fd.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 6)
                fd.setsockopt(socket.IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_PROBE)
                udp = struct.pack('!HHHH', 1234, 1234, 0, 0) + b' ' * 65528
                fd.sendto(udp, ('fd00::2', 0))

        ip('link del dummy', ns=ns)


if __name__ == "__main__":
    ksft_run([
        test_ipv6,
        test_ipv4,
        test_ipv6_jumbo,
    ])
    ksft_exit()
