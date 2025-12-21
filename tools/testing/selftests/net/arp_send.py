#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

import sys
from scapy.all import *

if len(sys.argv) != 9:
    print(f"Usage: {sys.argv[0]} <iface> <mac_dst> <mac_src> <op_code> <target-ip> <target-hwaddr> <sender-ip> <sender-hwaddr>\n");

iface = sys.argv[1]
mac_dst = sys.argv[2]
mac_src = sys.argv[3]
op = int(sys.argv[4])
tip = sys.argv[5]
tha = sys.argv[6]
sip = sys.argv[5]
sha = sys.argv[6]

pkt = (
    Ether(dst=mac_dst, src=mac_src) /
    ARP(op=op, psrc=sip, hwsrc=sha, pdst=tip, hwdst=tha)
)

sendp(pkt, iface=iface, verbose=False)
