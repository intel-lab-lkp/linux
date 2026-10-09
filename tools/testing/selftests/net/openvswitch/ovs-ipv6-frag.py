#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import ipaddress
import socket
import struct
import sys


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} INTERFACE", file=sys.stderr)
        return 2

    interface = sys.argv[1]
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

    with socket.socket(
        socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003)
    ) as sock:
        sock.bind((interface, 0))
        sent = sock.send(packet)

    if sent != len(packet):
        raise OSError(f"short packet send: {sent} of {len(packet)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
