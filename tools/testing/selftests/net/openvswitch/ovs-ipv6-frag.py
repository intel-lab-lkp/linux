#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import ipaddress
import os
import socket
import struct
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} DATAPATH", file=sys.stderr)
        return 2

    datapath = sys.argv[1]
    payload = bytes(range(8))
    # Offset 1 and M=1 make this a noninitial fragment.
    fragment = struct.pack("!BBHI", socket.IPPROTO_TCP, 0, 0x0009, 1)
    ipv6 = struct.pack(
        "!IHBB16s16s",
        6 << 28,
        len(fragment) + len(payload),
        socket.IPPROTO_FRAGMENT,
        64,
        ipaddress.IPv6Address("2001:db8::1").packed,
        ipaddress.IPv6Address("2001:db8::2").packed,
    )
    ethernet = struct.pack(
        "!6s6sH",
        b"\xff" * 6,
        bytes.fromhex("020000000001"),
        0x86DD,
    )
    packet = ethernet + ipv6 + fragment + payload

    ovs_dpctl = os.path.join(os.path.dirname(__file__), "ovs-dpctl.py")
    subprocess.run(
        [
            sys.executable,
            ovs_dpctl,
            "execute",
            datapath,
            packet.hex(),
            "eth(),eth_type(0x86dd),ipv6(proto=44)",
            "set(ipv6(src=2001:db8::99,proto=44,frag=2)),drop",
        ],
        check=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
