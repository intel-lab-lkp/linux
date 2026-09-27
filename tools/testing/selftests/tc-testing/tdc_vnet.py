#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Exercise TBF and police with a TCP GSO skb lacking a transport header."""

import fcntl
import json
import os
import struct
import subprocess
import sys
import time


# These architectures use a different _IOW direction encoding.
TUNSETIFF = (0x800454ca if os.uname().machine.startswith(
    ("alpha", "hppa", "mips", "parisc", "ppc", "sparc")) else 0x400454ca)
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000
IFF_VNET_HDR = 0x4000
TUN = "tuntdc0"
DEADLINE = time.monotonic() + 18


def run(*argv):
    remaining = DEADLINE - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("selftest deadline expired")
    return subprocess.check_output(argv, stderr=subprocess.STDOUT,
                                   text=True, timeout=min(3, remaining))


def tbf_drops(tc, ifb):
    qdiscs = json.loads(run(tc, "-s", "-j", "qdisc", "show", "dev", ifb,
                           "root"))
    return next(qdisc["drops"] for qdisc in qdiscs
                if qdisc["kind"] == "tbf" and qdisc["handle"] == "1:")


def wait_for_drops(tc, ifb, expected):
    deadline = min(DEADLINE, time.monotonic() + 5)
    while time.monotonic() < deadline:
        if tbf_drops(tc, ifb) >= expected:
            return
        time.sleep(0.05)
    raise RuntimeError(f"TBF did not reach {expected} drops")


def main(ip, tc, ifb):
    tun = os.open("/dev/net/tun", os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        ifreq = struct.pack("16sH", TUN.encode(),
                            IFF_TUN | IFF_NO_PI | IFF_VNET_HDR)
        fcntl.ioctl(tun, TUNSETIFF, ifreq)
        run(ip, "link", "set", "dev", TUN, "up")
        run(tc, "qdisc", "add", "dev", TUN, "clsact")
        run(tc, "filter", "add", "dev", TUN, "ingress", "pref", "1",
            "matchall", "action", "mirred", "egress", "redirect",
            "dev", ifb)

        # GSO without NEEDS_CSUM leaves transport_header unset. IPv4 IHL=0
        # prevents the later transport-header probe from filling it in.
        packet = struct.pack("=BBHHHH", 0, 1, 0, 8, 0, 0) + b"\x40" + bytes(999)
        if os.write(tun, packet) != len(packet):
            raise RuntimeError("short TUN write")
        wait_for_drops(tc, ifb, 1)

        run(tc, "filter", "delete", "dev", TUN, "ingress", "pref", "1")
        run(tc, "filter", "add", "dev", TUN, "ingress", "pref", "1",
            "matchall", "action", "police", "mtu", "65700",
            "conform-exceed", "pipe/drop", "action", "mirred", "egress",
            "redirect", "dev", ifb)
        if os.write(tun, packet) != len(packet):
            raise RuntimeError("short TUN write")
        wait_for_drops(tc, ifb, 2)
    finally:
        os.close(tun)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(f"usage: {sys.argv[0]} IP TC IFB")
    try:
        main(*sys.argv[1:])
    except (OSError, ValueError, RuntimeError, StopIteration,
            subprocess.SubprocessError) as error:
        sys.exit(f"tdc_vnet: {error}")
