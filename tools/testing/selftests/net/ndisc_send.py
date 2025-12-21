#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

import sys
from scapy.all import *

if len(sys.argv) != 9:
    print(f"Usage: {sys.argv[0]} <iface> <mac_dst> <mac_src> <ip_dst> <ip_src> <target_ip> <op> <lladr>")

iface = sys.argv[1]
mac_dst = sys.argv[2]
mac_src = sys.argv[3]
ip_dst = sys.argv[4]
ip_src = sys.argv[5]
tip = sys.argv[6]
op = int(sys.argv[7])
lladdr = sys.argv[8]

NDP_NA=136

if op == NDP_NA:
    pkt = (
        Ether(dst=mac_dst, src=mac_src) /
        IPv6(src=ip_src, dst=ip_dst, hlim=255) /
        ICMPv6ND_NA(R=0, S=0, O=1, tgt=tip) /
        ICMPv6NDOptDstLLAddr(lladdr=lladdr)
    )
else:
    pkt = (
        Ether(dst=mac_dst, src=mac_src) /
        IPv6(src=ip_src, dst=ip_dst, hlim=255) /
        ICMPv6ND_NS(tgt=tip) /
        ICMPv6NDOptSrcLLAddr(lladdr=lladdr)
    )

sendp(pkt, iface=iface, verbose=False)
