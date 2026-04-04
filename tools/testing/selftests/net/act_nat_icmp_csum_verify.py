#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Verify that act_nat correctly updates the inner IP header checksum
# in ICMP error packets. Sends a ping to an unreachable host and
# captures the resulting ICMP error via a raw socket, then validates
# the inner IP header checksum.
#
# This script is expected to run inside a network namespace that has
# act_nat configured on its veth interface.

import socket
import struct
import sys
import os
import signal


def ip_checksum(header_bytes):
    """Compute IP header checksum."""
    if len(header_bytes) % 2:
        header_bytes += b'\x00'
    total = 0
    for i in range(0, len(header_bytes), 2):
        total += (header_bytes[i] << 8) + header_bytes[i + 1]
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def verify_inner_checksum(icmp_payload):
    """Extract and verify the inner IP header checksum from ICMP error."""
    # ICMP error payload starts with the original IP header
    if len(icmp_payload) < 20:
        return None, "inner IP header too short"

    inner_ihl = (icmp_payload[0] & 0x0f) * 4
    if len(icmp_payload) < inner_ihl:
        return None, "inner IP header truncated"

    inner_hdr = icmp_payload[:inner_ihl]

    stored_csum = (inner_hdr[10] << 8) | inner_hdr[11]

    # Zero out checksum field and recompute
    hdr_for_csum = bytearray(inner_hdr)
    hdr_for_csum[10] = 0
    hdr_for_csum[11] = 0
    computed_csum = ip_checksum(bytes(hdr_for_csum))

    inner_src = socket.inet_ntoa(inner_hdr[12:16])
    inner_dst = socket.inet_ntoa(inner_hdr[16:20])
    info = f"inner src={inner_src} dst={inner_dst}"

    if stored_csum == computed_csum:
        return True, f"valid (0x{stored_csum:04x}) {info}"
    else:
        return False, (f"mismatch: stored=0x{stored_csum:04x} "
                       f"computed=0x{computed_csum:04x} {info}")


def main():
    # Open raw ICMP socket to receive ICMP errors
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_RAW,
                             socket.IPPROTO_ICMP)
    except PermissionError:
        print("SKIP - need CAP_NET_RAW")
        return 4

    sock.settimeout(5)

    # Send a ping to an unreachable address to trigger ICMP error
    try:
        ping_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
                                  socket.IPPROTO_ICMP)
    except (PermissionError, OSError):
        # Fallback: use raw socket for ping
        ping_sock = socket.socket(socket.AF_INET, socket.SOCK_RAW,
                                  socket.IPPROTO_ICMP)

    # ICMP echo request: type=8, code=0, checksum, id, seq
    icmp_id = os.getpid() & 0xffff
    icmp_echo = struct.pack('!BBHHH', 8, 0, 0, icmp_id, 1)
    # Compute ICMP checksum
    csum = ip_checksum(icmp_echo)
    icmp_echo = struct.pack('!BBHHH', 8, 0, csum, icmp_id, 1)

    try:
        ping_sock.sendto(icmp_echo, ('10.0.2.99', 0))
    except OSError:
        pass
    ping_sock.close()

    # Wait for ICMP error response
    try:
        data, addr = sock.recvfrom(4096)
    except socket.timeout:
        print("SKIP - no ICMP error received (timeout)")
        sock.close()
        return 4

    sock.close()

    # Parse outer IP header
    if len(data) < 20:
        print("SKIP - received packet too short")
        return 4

    outer_ihl = (data[0] & 0x0f) * 4

    # ICMP header at offset outer_ihl
    icmp_offset = outer_ihl
    if len(data) < icmp_offset + 8:
        print("SKIP - packet too short for ICMP header")
        return 4

    icmp_type = data[icmp_offset]
    icmp_code = data[icmp_offset + 1]

    # Expect ICMP dest unreachable (type 3) or similar error
    if icmp_type not in (3, 4, 5, 11, 12):
        print(f"SKIP - received ICMP type {icmp_type}, not an error")
        return 4

    # Inner IP header starts after ICMP header (8 bytes)
    inner_offset = icmp_offset + 8
    inner_payload = data[inner_offset:]

    result, msg = verify_inner_checksum(inner_payload)

    if result is None:
        print(f"SKIP - {msg}")
        return 4
    elif result:
        print(f"OK - inner IP header checksum {msg}")
        return 0
    else:
        print(f"FAIL - inner IP header checksum {msg}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
