#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test that bond_header_parse() does not infinitely recurse with stacked bonds.
#
# When a non-Ethernet device (e.g. GRE) is enslaved to a bond that is itself
# enslaved to another bond (bond1 -> bond0 -> gre), receiving a packet via
# AF_PACKET SOCK_DGRAM triggers dev_parse_header() -> bond_header_parse().
# Since parse() used skb->dev (always the topmost bond) instead of a passed-in
# dev pointer, it would recurse back into itself indefinitely.

ALL_TESTS="
	bond_test_stacked_header_parse
"
REQUIRE_MZ=no
NUM_NETIFS=0
lib_dir=$(dirname "$0")
source "$lib_dir"/../../../net/forwarding/lib.sh

require_command()
{
	if ! command -v "$1" &>/dev/null; then
		echo "SKIP: $1 is not installed"
		exit "$ksft_skip"
	fi
}

bond_test_stacked_header_parse()
{
	local devdummy="test-dummy0"
	local devgre="test-gre0"
	local devbond0="test-bond0"
	local devbond1="test-bond1"

	RET=0

	# Setup: dummy -> gre -> bond0 -> bond1
	modprobe dummy 2>/dev/null
	modprobe ip_gre 2>/dev/null
	modprobe bonding 2>/dev/null

	ip link add name "$devdummy" type dummy
	if [ $? -ne 0 ]; then
		log_test_skip "could not create dummy device (CONFIG_DUMMY)"
		return
	fi
	ip addr add 10.0.0.1/24 dev "$devdummy"
	ip link set "$devdummy" up

	ip link add name "$devgre" type gre local 10.0.0.1
	if [ $? -ne 0 ]; then
		log_test_skip "could not create GRE device (CONFIG_NET_IPGRE)"
		ip link del "$devdummy" 2>/dev/null
		return
	fi

	ip link add name "$devbond0" type bond mode active-backup
	check_err $? "could not create bond0"
	ip link add name "$devbond1" type bond mode active-backup
	check_err $? "could not create bond1"

	ip link set "$devgre" master "$devbond0"
	check_err $? "could not enslave $devgre to $devbond0"
	ip link set "$devbond0" master "$devbond1"
	check_err $? "could not enslave $devbond0 to $devbond1"

	ip link set "$devgre" up
	ip link set "$devbond0" up
	ip link set "$devbond1" up

	# Send a GRE-encapsulated packet to 10.0.0.1 while an AF_PACKET
	# SOCK_DGRAM socket is listening on bond1. The receive path calls
	# dev_parse_header() which invokes bond_header_parse(). With the
	# bug, this recurses infinitely and causes a stack overflow.
	#
	# Use Python to:
	# 1. Open AF_PACKET SOCK_DGRAM on bond1
	# 2. Send a GRE packet to 10.0.0.1 via raw socket
	# 3. Try to receive (triggers parse path)
	python3 -c "
import socket, struct, time

# AF_PACKET SOCK_DGRAM on bond1
ETH_P_ALL = 0x0003
pkt_fd = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM,
                       socket.htons(ETH_P_ALL))
pkt_fd.settimeout(2)
pkt_fd.bind(('$devbond1', ETH_P_ALL))

# Build GRE-encapsulated IP packet
def build_ip_hdr(proto, saddr, daddr, payload_len):
    ihl_ver = 0x45
    total_len = 20 + payload_len
    hdr = struct.pack('!BBHHHBBH4s4s',
        ihl_ver, 0, total_len, 0, 0, 64, proto, 0,
        socket.inet_aton(saddr), socket.inet_aton(daddr))
    # compute checksum
    words = struct.unpack('!10H', hdr)
    s = sum(words)
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    chksum = ~s & 0xffff
    hdr = hdr[:10] + struct.pack('!H', chksum) + hdr[12:]
    return hdr

inner = build_ip_hdr(17, '192.168.1.1', '192.168.1.2', 8) + b'\x00' * 8
gre_hdr = struct.pack('!HH', 0, 0x0800)  # flags=0, proto=IP
outer = build_ip_hdr(47, '10.0.0.2', '10.0.0.1', len(gre_hdr) + len(inner))
pkt = outer + gre_hdr + inner

raw_fd = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
raw_fd.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
raw_fd.sendto(pkt, ('10.0.0.1', 0))
raw_fd.close()

try:
    pkt_fd.recv(2048)
except socket.timeout:
    pass
pkt_fd.close()
" 2>/dev/null

	# If we get here without a kernel crash/hang, the test passed.
	# Also check dmesg for signs of the recursion bug.
	if dmesg | tail -20 | grep -q "BUG: MAX_LOCK_DEPTH\|stack-overflow\|stack overflow"; then
		check_err 1 "kernel detected recursion in bond_header_parse"
	fi

	# Cleanup
	ip link del "$devbond1" 2>/dev/null
	ip link del "$devbond0" 2>/dev/null
	ip link del "$devgre" 2>/dev/null
	ip link del "$devdummy" 2>/dev/null

	log_test "Stacked bond header_parse does not recurse"
}

require_command python3

tests_run

exit "$EXIT_STATUS"
