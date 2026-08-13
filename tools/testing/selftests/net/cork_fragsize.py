#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Test possible UDP length overflow in udp_send_skb/udp_v6_send_skb.

from lib.py import ksft_run, ksft_exit, ksft_true
from lib.py import ip, NetNS, NetNSEnter
import errno
import socket
import subprocess


IP_MTU_DISCOVER = 10
IP_PMTUDISC_PROBE = 3
IPV6_MTU_DISCOVER = 23
IPV6_PMTUDISC_DO = 2


def check_dmesg_clean(func) -> bool:
    dmesg = subprocess.Popen(['dmesg'], stdout=subprocess.PIPE)
    result = subprocess.run(['grep', '-q', f'WARNING:.*{func}'], stdin=dmesg.stdout)
    dmesg.wait()
    return result.returncode != 0 and dmesg.returncode == 0


def test_ipv6() -> None:
    with NetNS() as ns:
        ip('link add dummy type dummy', ns=ns)
        ip('link set dummy mtu 65576', ns=ns)
        ip('link set dummy up', ns=ns)
        ip('-6 addr add fd00::1/64 dev dummy nodad', ns=ns)
        ip('-6 neigh add fd00::2 lladdr 02:00:00:00:00:02 dev dummy nud permanent', ns=ns)

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
    with NetNS() as ns:
        ip('link add dummy type dummy', ns=ns)
        ip('link set dummy mtu 65556', ns=ns)
        ip('link set dummy up', ns=ns)
        ip('addr add 10.0.0.1/24 dev dummy', ns=ns)
        ip('neigh add 10.0.0.2 lladdr 02:00:00:00:00:02 dev dummy nud permanent', ns=ns)

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


if __name__ == "__main__":
    ksft_run([
        test_ipv6,
        test_ipv4,
    ])
    ksft_exit()
