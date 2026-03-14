#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Test of extension header limits

import getopt
import struct
import sys
import socket
import scapy.all
import proto_nums
import ext_hdr

# Constants

VERBOSE = False
SOURCE_MAC = "00:11:22:33:44:55"
DESTINATION_MAC = "AA:BB:CC:DD:EE:FF"
SOURCE_IP = "2001:db8::7"
DESTINATION_IP = "2001:db8::8"
PACKET_LIST = []
PCAP_OUT=""
GLOB_IDENT = 1111
ETHER_FRAME = scapy.all.Raw()
WITH_ETH = False

# Parse command line options
def cli_args():
    global VERBOSE, SOURCE_MAC, DESTINATION_MAC, SOURCE_IP
    global DESTINATION_IP, PCAP_OUT

    args = sys.argv[1:]

    try:
        opts, args = getopt.getopt(args, "vw:",
                        ["verbose", "src_eth=", "dst_eth", "src_ip=",
                         "dst_ip=", "pcap_out="])
    except getopt.GetoptError as err:
        # Print error message and exit
        print(err)
        sys.exit(2)

    for opt, arg in opts:
        if opt in ("-v", "--verbose"):
            VERBOSE = True
        elif opt in ("--src_eth"):
            SOURCE_MAC = arg
        elif opt in ("--dst_eth"):
            DESTINATION_MAC = arg
        elif opt in ("--src_ip"):
            SOURCE_IP = arg
        elif opt in ("--dst_ip"):
            DESTINATION_IP = arg
        elif opt in ("-w", "--pcap_out"):
            PCAP_OUT = arg

# Make an ICMP echo request packet with the requested Extension Header chain
def make_packet(text_name, eh_list):
    global GLOB_IDENT

    hdr = scapy.all.Raw()
    plen = 0

    hdr = scapy.all.ICMPv6EchoRequest(id=GLOB_IDENT)/hdr
    plen += 8

    pair = ext_hdr.make_eh_chain(
            proto_nums.IP_Proto.IP_PROTO_IPv6_ICMP.value, eh_list)
    hdr = pair[0]/hdr
    plen += pair[1]

    ipv6_pkt = scapy.all.IPv6(src=SOURCE_IP, dst=DESTINATION_IP,
                              nh=pair[2], plen=plen)
    hdr = ipv6_pkt / hdr
    plen += 40

    if WITH_ETH:
        hdr = ETHER_FRAME/hdr
        plen += 14

    PACKET_LIST.append((hdr, plen, GLOB_IDENT, pair[3], text_name))
    GLOB_IDENT += 1

# Write a pacp file with all the created packets
def write_pcap(pcap_out):
    global ETHER_FRAME, WITH_ETH

    ETHER_FRAME = scapy.all.Ether(src=SOURCE_MAC, dst=DESTINATION_MAC,
                                  type=0x86DD)
    WITH_ETH = True

    packets=[]
    for packet in PACKET_LIST:
        packets.append(packet[0])

    scapy.all.wrpcap(pcap_out, packets)

def process_return(recvd_it, packet):
    if VERBOSE:
        if recvd_it:
            if packet[3]:
                print(f"TEST: {packet[4]}: Received as expected")
            else:
                print(f"TEST: {packet[4]}: Unexpected receive")
        else:
            if packet[3]:
                print(f"TEST: {packet[4]}: Didn't receive, "
                      "but receive expected")
            else:
                print(f"TEST: {packet[4]}: Didn't receive as expected")

    if (recvd_it and packet[3] is not True):
        # We got a reply but weren't expecting one
        print(f"FAIL: Receive was unexpected for {packet[4]}")
        return False

    if (not recvd_it and packet[3]):
        # We didn't get a reply but weret expecting one
        print(f"FAIL: Expected to receive for {packet[4]}")
        return False

    return True

# Run ping test
def run_test():
    # Open raw ICMP socket
    try:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_RAW,
                  socket.IPPROTO_ICMPV6)
    except PermissionError:
        print("This script requires root privileges.")
        return 2

    # Bind to interface by its IP address
    sock.bind((SOURCE_IP, 0))

    # Run through each packet
    for packet in PACKET_LIST:
        # Send packet
        scapy.all.send(packet[0], verbose=False)

        sock.settimeout(0.100)
        recvd_it = False

        # Try to get ICMP echo reply
        try:
            while not recvd_it:
                rpacket, _addr = sock.recvfrom(1024)
                icmp_type = rpacket[0]
                identifier = struct.unpack(">H", rpacket[4:6])
                if (icmp_type ==
                    proto_nums.ICMP6_Type.ICMPV6_ECHO_REPLY.value and
                    identifier[0] == packet[2]):
                    recvd_it = True

        except socket.timeout:
            pass

        process_return(recvd_it, packet)

    return 0

# Make packets for various test cases
def make_test_packets():
    # Two non-padding options in HBH and DestOpt, should succeed
    # with default sysctls
    make_packet("Two non-padding options in HBH and DestOpts",
        [
            ("H", [(11, 4), (0, 0), (0, 0), (12, 3)]),
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3)]),
        ])

    # Big destination option, should fail when
    # net.ipv6.max_dst_opts_length equals 64
    make_packet("Big destination option",
        [
            ("H", [(11, 4), (0, 0), (0, 0), (12, 3)]),
            ("D", [(1, 4), (12, 255)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
        ])

    # Almost Big HBH option should succeed when
    # net.ipv6.max_hbh_length equals 64
    make_packet("Almost Big HBH option",
        [
            ("H", [(11, 53), (1, 0), (12, 3)]),
            ("D", [(1, 4), (12, 1)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
        ])

    # Big Hop-by-Hop option, should fail when
    # net.ipv6.max_hbh_length equals 64
    make_packet("Big HBH option",
        [
            ("H", [(11, 53), (1, 0), (0, 0), (12, 3)]),
            ("D", [(1, 4), (12, 1)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
        ])

    # Too much HBH padding, should always fail
    make_packet("Too much HBH padding",
        [
            ("H", [(12, 3), (1, 8)]),
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3)]),
        ])

    # Too much DestOpt padding, should always fail
    make_packet("Too much DestOpt padding",
        [
            ("H", [(12, 3), (1, 8)]),
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3), (0, 0), (1, 6), (0, 0), (12, 3)]),
        ])

    # Too much DestOpt padding, should always fail
    make_packet("Too much DestOpt padding #2",
        [
            ("H", [(12, 3)]),
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3), (0, 0), (0, 0), (0, 0), (0, 0),
                   (0, 0), (0, 0), (0, 0), (0, 0), (12, 3)]),
        ])

    # Too much DestOpt padding, should always fail

    make_packet("Too much DestOpt padding #3",
        [
            ("D", [(0, 0), (0, 0), (0, 0), (0, 0),
                   (0, 0), (0, 0), (0, 0), (0, 0)]),
        ])

    # Almost too much DestOpt padding, should succeed with default
    # sysctl settings
    make_packet("Almost too much DestOpt padding #2",
        [
            ("H", [(12, 3)]),
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3), (0, 0), (0, 0), (0, 0), (0, 0),
                   (0, 0), (0, 0), (0, 0), (12, 3)]),
        ])

    # Two Dest Ops, should fail unless net.ipv6.enforce_ext_hdr_order
    # equals 1
    make_packet("Two Dest Ops",
        [
            ("D", []),
            ("D", []),
        ])

    # OOO Routing headers, should fail unless
    # net.ipv6.enforce_ext_hdr_order equals 1
    make_packet("OOO Routing header",
        [
            ("F", (0x89abcdef)),
            ("R", [ "888::1", "9999::1"]),
        ])

    # Two Routing headers, should fail unless
    # net.ipv6.enforce_ext_hdr_order equals 1
    make_packet("Two Routing headers",
        [
            ("R", [ "888::1", "9999::1"]),
            ("R", [ "888::1", "9999::1"]),
        ])

    # Two DestOpt headers with Routing header should succeed with default
    # sysctl settings
    make_packet("Two Destination options okay",
        [
            ("D", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3)]),
        ])

    # Two DestOpt headers without Routing header should fail unless
    # net.ipv6.enforce_ext_hdr_order equals 1
    make_packet("Two Destination options",
        [
            ("D", [(1, 4), (12, 3)]),
            ("D", [(1, 4), (12, 3)]),
        ])

    # Two DestOpt headers after Routing header, should fail unless
    # net.ipv6.enforce_ext_hdr_order equals 1
    make_packet("Two Destination options after RH",
        [
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3)]),
            ("D", [(1, 4), (12, 3)]),
        ])

    # Many extension headers, should fail unless
    # net.ipv6.enforce_ext_hdr_order equals 1
    make_packet("Many EH OOO",
        [
            ("H", [(1, 4), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3)]),
            ("D", [(1, 4), (12, 3), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3)]),
            ("F", (0x89abcdef)),
            ("D", [(1, 4), (12, 3), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3), (12, 3)]),
            ("R", [ "888::1", "9999::1"]),
            ("R", [ "888::1", "9999::1"]),
            ("D", [(1, 4), (12, 3), (12, 3)]),
        ])

    # Two fragment headers, should always fail due to stack
    # implementation
    make_packet("Two fragment Headers",
        [
            ("F", (0x89abcdef)),
            ("F", (0x89abcdef)),
        ])

cli_args()

make_test_packets()

if PCAP_OUT != "":
    write_pcap(PCAP_OUT)
    STATUS = 0
else:
    STATUS = run_test()

sys.exit(STATUS)
