#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Test of extension header limits

import ext_hdr
import getopt
import proto_nums
import shlex

from scapy.all import *

# Constants

verbose = False
source_mac = "00:11:22:33:44:55"
destination_mac = "AA:BB:CC:DD:EE:FF"
source_ip = "2001:db8::7"
destination_ip = "2001:db8::8"
packet_list = []
pcap_out=""
glob_ident = 1111
ether_frame = Raw()
with_eth = False

# Parse command line options
def Cli_Args():
	global verbose, source_mac, destination_mac, source_ip
	global destination_ip, pcap_out

	args = sys.argv[1:]

	try:
		opts, remainder = getopt.getopt(args, "vw:",
			["verbose", "src_eth=", "dst_eth", "src_ip=",
			 "dst_ip=", "pcap_out="])
	except getopt.GetoptError as err:
		# Print error message and exit
		print(err)
		sys.exit(2)

	for opt, arg in opts:
		if opt in ("-v", "--verbose"):
			verbose = True
		elif opt in ("--src_eth"):
			source_mac = arg
		elif opt in ("--dst_eth"):
			destination_mac = arg
		elif opt in ("--src_ip"):
			source_ip = arg
		elif opt in ("--dst_ip"):
			destination_ip = arg
		elif opt in ("-w", "--pcap_out"):
			pcap_out = arg

# Make an ICMP echo request packet with the requested Extension Header chain
def Make_Packet(text_name, eh_list):
	global glob_ident,  packet_list, with_eth

	hdr = Raw()
	len = 0

	hdr = ICMPv6EchoRequest(id=glob_ident)/hdr
	len += 8

	pair = ext_hdr.Make_EH_Chain(
		proto_nums.IP_Proto.IP_PROTO_IPv6_ICMP.value, eh_list)
	hdr = pair[0]/hdr
	len += pair[1]

	ipv6_pkt = IPv6(src=source_ip, dst=destination_ip, nh=pair[2], plen=len)
	hdr = ipv6_pkt / hdr
	len += 40

	if (with_eth):
		hdr = ether_frame/hdr
		len += 14

	packet_list.append((hdr, len, glob_ident, pair[3], text_name))
	glob_ident += 1

# Write a pacp file with all the created packets
def Write_Pcap(pcap_out):
	global packet_list, ether_frame, with_eth

	ether_frame = Ether(src=source_mac, dst=destination_mac, type=0x86DD)
	with_eth = True

	packets=[]
	for packet in packet_list:
		packets.append(packet[0])
	wrpcap(pcap_out, packets)

# Run ping test
def Run_Test():
	global packet_list, verbose, source_ip, destination_ip

	# Open raw ICMP socket
	try:
	    s = socket.socket(socket.AF_INET6, socket.SOCK_RAW,
			      socket.IPPROTO_ICMPV6)
	except PermissionError:
		print("This script requires root privileges.")
		return 2

	# Bind to interface by its IP address
	s.bind((source_ip, 0))

	# Run through eACH PACKET
	for packet in packet_list:
		# Send packet
		send(packet[0], verbose=False)

		s.settimeout(0.100)
		found_it = False

		# Try to get ICMP echo reply
		try:
			while (found_it == False):
				rpacket, addr = s.recvfrom(1024)
				x = struct.unpack(">H", rpacket[0:2])
				icmp_type, icmp_code = struct.unpack("BB",
								rpacket[0:2])
				checksum = struct.unpack("H", rpacket[2:4])
				identifier = struct.unpack(">H", rpacket[4:6])
				seqno = struct.unpack(">H", rpacket[6:8])
				if (icmp_type ==
				    proto_nums.ICMP6_Type.ICMPV6_ECHO_REPLY.value and
				    identifier[0] == packet[2]):
					found_it = True

		except socket.timeout:
			pass

		if (verbose):
			if (found_it):
				if (packet[3]):
					print("TEST: %s: Received as expected" %
								packet[4])
				else:
					print("TEST: %s: Unexpedted receive" %
								packet[4])
			else:
				if (packet[3]):
					print("TEST: %s: Didn't receive, "
					      "but receive expected" %
								packet[4])
				else:
					print("TEST: %s: Didn't receive as "
					      "expected" % packet[4])


		if (found_it and packet[3] != True):
			# We got a reply but weren't expecting one
			print("FAIL: Receive was unexpected for %s" % packet[4])
			return 1
		elif (not found_it and packet[3]):
			# We didn't get a reply but weret expecting one
			print("FAIL: Expected to receive for %s" % packet[4])
			return 1

	return 0

# Make packets for various test cases
def Make_Test_Packets():
	global packet_list

	# Two non-padding options in HBH and DestOpt, should succeed
	# with default sysctls
	Make_Packet("Two non-padding options in HBH and DestOpts",
	    [
		("H", [(11, 4), (0, 0), (0, 0), (12, 3)]),
		("D", [(1, 4), (12, 3)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
		("D", [(1, 4), (12, 3)]),
	    ])

	# Big destination option, should fail when
	# net.ipv6.max_dst_opts_length equals 64
	Make_Packet("Big destination option",
	    [
		("H", [(11, 4), (0, 0), (0, 0), (12, 3)]),
		("D", [(1, 4), (12, 255)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
	    ])

	# Almost Big HBH option should succeed when
	# net.ipv6.max_hbh_length equals 64
	Make_Packet("Almost Big HBH option",
	    [
		("H", [(11, 53), (1, 0), (12, 3)]),
		("D", [(1, 4), (12, 1)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
	    ])

	# Big Hop-by-Hop option, should fail when
	# net.ipv6.max_hbh_length equals 64
	Make_Packet("Big HBH option",
	    [
		("H", [(11, 53), (1, 0), (0, 0), (12, 3)]),
		("D", [(1, 4), (12, 1)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
	    ])

	# Too much HBH padding, should always fail
	Make_Packet("Too much HBH padding",
	    [
		("H", [(12, 3), (1, 8)]),
		("D", [(1, 4), (12, 3)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
		("D", [(1, 4), (12, 3)]),
	    ])

	# Too much DestOpt padding, should always fail
	Make_Packet("Too much DestOpt padding",
	    [
		("H", [(12, 3), (1, 8)]),
		("D", [(1, 4), (12, 3)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
		("D", [(1, 4), (12, 3), (0, 0), (1, 6), (0, 0), (12, 3)]),
	    ])

	# Too much DestOpt padding, should always fail
	Make_Packet("Too much DestOpt padding #2",
	    [
		("H", [(12, 3)]),
		("D", [(1, 4), (12, 3)]),
		("R", [ "888::1", "9999::1"]),
		("F", (0x89abcdef)),
		("D", [(1, 4), (12, 3), (0, 0), (0, 0), (0, 0), (0, 0),
		       (0, 0), (0, 0), (0, 0), (0, 0), (12, 3)]),
	    ])

	# Too much DestOpt padding, should always fail

	Make_Packet("Too much DestOpt padding #3",
	    [
		("D", [(0, 0), (0, 0), (0, 0), (0, 0),
		       (0, 0), (0, 0), (0, 0), (0, 0)]),
	    ])

	# Almost too much DestOpt padding, should succeed with default
	# sysctl settings
	Make_Packet("Almost too much DestOpt padding #2",
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
	Make_Packet("Two Dest Ops",
	    [
		("D", []),
		("D", []),
	    ])

	# OOO Routing headers, should fail unless
	# net.ipv6.enforce_ext_hdr_order equals 1
	Make_Packet("OOO Routing header",
	    [
		("F", (0x89abcdef)),
		("R", [ "888::1", "9999::1"]),
	    ])

	# Two Routing headers, should fail unless
	# net.ipv6.enforce_ext_hdr_order equals 1
	Make_Packet("Two Routing headers",
	    [
		("R", [ "888::1", "9999::1"]),
		("R", [ "888::1", "9999::1"]),
	    ])

	# Two DestOpt headers with Routing header should succeed with default
	# sysctl settings
	Make_Packet("Two Destination options okay",
	    [
		("D", [(1, 4), (12, 3)]),
		("R", [ "888::1", "9999::1"]),
		("D", [(1, 4), (12, 3)]),
	    ])

	# Two DestOpt headers without Routing header should fail unless
	# net.ipv6.enforce_ext_hdr_order equals 1
	Make_Packet("Two Destination options",
	    [
		("D", [(1, 4), (12, 3)]),
		("D", [(1, 4), (12, 3)]),
	    ])

	# Two DestOpt headers after Routing header, should fail unless
	# net.ipv6.enforce_ext_hdr_order equals 1
	Make_Packet("Two Destination options after RH",
	    [
		("R", [ "888::1", "9999::1"]),
		("D", [(1, 4), (12, 3)]),
		("D", [(1, 4), (12, 3)]),
	    ])

	# Many extension headers, should fail unless
	# net.ipv6.enforce_ext_hdr_order equals 1
	Make_Packet("Many EH OOO",
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
	Make_Packet("Two fragment Headers",
	    [
		("F", (0x89abcdef)),
		("F", (0x89abcdef)),
	    ])

Cli_Args()

Make_Test_Packets()

if (pcap_out != ""):
	Write_Pcap(pcap_out)
	status = 0
else:
	status = Run_Test()

exit(status)
