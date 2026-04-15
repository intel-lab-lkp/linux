#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test that UDP encapsulation (FOU) correctly handles packet resubmit
# when packets are delivered via the multicast UDP delivery path.
#
# When a FOU-encapsulated packet arrives with a multicast destination IP,
# __udp4_lib_mcast_deliver() must resubmit it to the inner protocol
# handler (e.g., GRE) rather than consuming it. This test verifies that
# by creating a FOU/GRETAP tunnel with a multicast remote address, sending
# encapsulated packets, and checking that they are correctly decapsulated.
#
# The early demux optimization can mask this issue by routing packets via
# the unicast path (udp_unicast_rcv_skb), so we disable it to force
# packets through __udp4_lib_mcast_deliver().

source lib.sh

NSENDER=""
NRECV=""

cleanup() {
	cleanup_all_ns
}

trap cleanup EXIT

setup() {
	setup_ns NSENDER NRECV

	ip link add veth_s type veth peer name veth_r
	ip link set veth_s netns "$NSENDER"
	ip link set veth_r netns "$NRECV"

	ip -n "$NSENDER" addr add 10.0.0.1/24 dev veth_s
	ip -n "$NSENDER" link set veth_s up

	ip -n "$NRECV" addr add 10.0.0.2/24 dev veth_r
	ip -n "$NRECV" link set veth_r up

	# Disable early demux to force multicast delivery path
	ip netns exec "$NRECV" sysctl -wq net.ipv4.ip_early_demux=0

	# Join multicast group on receiver
	ip -n "$NRECV" addr add 239.0.0.1/32 dev veth_r autojoin

	# Multicast routes
	ip -n "$NRECV" route add 239.0.0.0/8 dev veth_r
	ip -n "$NSENDER" route add 239.0.0.0/8 dev veth_s

	# FOU listener
	ip netns exec "$NRECV" ip fou add port 4797 ipproto 47

	# GRETAP with multicast remote - this triggers __udp4_lib_mcast_deliver
	ip -n "$NRECV" link add eoudp0 type gretap \
		remote 239.0.0.1 local 10.0.0.2 \
		encap fou encap-sport 4797 encap-dport 4797 \
		key 239.0.0.1
	ip -n "$NRECV" link set eoudp0 up
	ip -n "$NRECV" addr add 192.168.99.2/24 dev eoudp0
}

send_fou_gre_packets() {
	local count=$1
	local script_dir
	script_dir=$(dirname "$(readlink -f "$0")")

	ip netns exec "$NSENDER" python3 "$script_dir/fou_mcast_encap.py" \
		-c "$count" -d 239.0.0.1 -p 4797
}

get_rx_packets() {
	ip -n "$NRECV" -s link show eoudp0 | awk '/RX:/{getline; print $2}'
}

test_fou_mcast_encap() {
	local count=100
	local rx_before
	local rx_after
	local rx_delta

	rx_before=$(get_rx_packets)
	send_fou_gre_packets $count
	sleep 1
	rx_after=$(get_rx_packets)

	rx_delta=$((rx_after - rx_before))

	if [ "$rx_delta" -ge "$count" ]; then
		echo "PASS: received $rx_delta/$count packets via multicast FOU/GRETAP"
		return "$ksft_pass"
	elif [ "$rx_delta" -gt 0 ]; then
		echo "FAIL: only $rx_delta/$count packets received (partial delivery)"
		return "$ksft_fail"
	else
		echo "FAIL: 0/$count packets received (multicast encap resubmit broken)"
		return "$ksft_fail"
	fi
}

echo "TEST: FOU/GRETAP multicast encapsulation resubmit"

setup
test_fou_mcast_encap
exit $?
