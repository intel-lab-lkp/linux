#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Send FOU/GRE encapsulated packets to a multicast destination.

Build and send GRE-over-UDP (FOU) packets with a multicast outer
destination IP. Used by fou_mcast_encap.sh to test that the UDP
multicast delivery path correctly resubmits encapsulated packets
to the inner protocol handler.
"""

import argparse
import socket
import struct


def checksum(data):
    """Compute Internet checksum (RFC 1071)."""
    csum = 0
    for i in range(0, len(data) - 1, 2):
        csum += (data[i] << 8) + data[i + 1]
    if len(data) % 2:
        csum += data[-1] << 8
    while csum >> 16:
        csum = (csum & 0xFFFF) + (csum >> 16)
    return ~csum & 0xFFFF


def build_gre_encap_packet(dst_addr):
    """Build a GRE/Ethernet/IP/ICMP payload for FOU encapsulation."""
    gre_key = socket.inet_aton(dst_addr)
    gre_hdr = struct.pack("!HH", 0x2000, 0x6558) + gre_key

    dst_mac = b"\xff\xff\xff\xff\xff\xff"
    src_mac = b"\x02\x00\x00\x00\x00\x01"
    eth_hdr = dst_mac + src_mac + struct.pack("!H", 0x0800)

    inner_ip_src = socket.inet_aton("192.168.99.1")
    inner_ip_dst = socket.inet_aton("192.168.99.2")

    icmp_payload = b"TESTFOU!" * 4
    icmp_hdr = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + icmp_payload
    icmp_csum = checksum(icmp_hdr)
    icmp_hdr = struct.pack("!BBHHH", 8, 0, icmp_csum, 0x1234, 1) + icmp_payload

    ip_len = 20 + len(icmp_hdr)
    ip_hdr = struct.pack("!BBHHHBBH", 0x45, 0, ip_len, 0x1234, 0, 64, 1, 0)
    ip_hdr += inner_ip_src + inner_ip_dst
    ip_csum = checksum(ip_hdr)
    ip_hdr = ip_hdr[:10] + struct.pack("!H", ip_csum) + ip_hdr[12:]

    return gre_hdr + eth_hdr + ip_hdr + icmp_hdr


def main():
    parser = argparse.ArgumentParser(
        description="Send FOU/GRE encapsulated packets to a multicast address"
    )
    parser.add_argument(
        "-c", "--count", type=int, required=True,
        help="number of packets to send",
    )
    parser.add_argument(
        "-d", "--dst", default="239.0.0.1",
        help="destination multicast address (default: 239.0.0.1)",
    )
    parser.add_argument(
        "-p", "--port", type=int, default=4797,
        help="destination UDP port (default: 4797)",
    )
    args = parser.parse_args()

    payload = build_gre_encap_packet(args.dst)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(args.count):
        sock.sendto(payload, (args.dst, args.port))
    sock.close()


if __name__ == "__main__":
    main()
