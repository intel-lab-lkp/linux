#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Testing for IPv4 and IPv6 BIG TCP over VXLAN and GENEVE tunnels.

SERVER_NS=$(mktemp -u server-XXXXXXXX)
SERVER_IP4="192.168.1.1"
SERVER_IP6="2001:db8::1:1"
SERVER_IP4_TUN="192.168.2.1"
SERVER_IP6_TUN="2001:db8::2:1"

CLIENT_NS=$(mktemp -u client-XXXXXXXX)
CLIENT_IP4="192.168.1.2"
CLIENT_IP6="2001:db8::1:2"
CLIENT_IP4_TUN="192.168.2.2"
CLIENT_IP6_TUN="2001:db8::2:2"

: "${PACKETS_THRESHOLD:=1000}"

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

setup() {
	ip netns add "$SERVER_NS"
	ip netns add "$CLIENT_NS"
	ip -netns "$SERVER_NS" link add link1 type veth peer name link0 netns "$CLIENT_NS"

	ip -netns "$CLIENT_NS" link set link0 up
	ip -netns "$CLIENT_NS" addr replace "$CLIENT_IP4/24" dev link0
	ip -netns "$CLIENT_NS" addr replace "$CLIENT_IP6/112" dev link0 nodad
	ip -netns "$CLIENT_NS" link set link0 \
		gso_max_size 196608 gso_ipv4_max_size 196608 \
		gro_max_size 196608 gro_ipv4_max_size 196608
	ip -netns "$SERVER_NS" link set link1 up
	ip -netns "$SERVER_NS" addr replace "$SERVER_IP4/24" dev link1
	ip -netns "$SERVER_NS" addr replace "$SERVER_IP6/112" dev link1 nodad
	ip -netns "$SERVER_NS" link set link1 \
		gso_max_size 196608 gso_ipv4_max_size 196608 \
		gro_max_size 196608 gro_ipv4_max_size 196608

	ip netns exec "$SERVER_NS" netserver >/dev/null
}

setup_tunnel() {
	if [ "$2" = 4 ]; then
		SERVER_IP="$SERVER_IP4"
		CLIENT_IP="$CLIENT_IP4"
		echo "Setting up ${1^^} over IPv4, veth tx csum offload $3"
	else
		SERVER_IP="$SERVER_IP6"
		CLIENT_IP="$CLIENT_IP6"
		echo "Setting up ${1^^} over IPv6, veth tx csum offload $3"
	fi

	if [ "$1" = vxlan ]; then
		ip -netns "$CLIENT_NS" link add tun0 type vxlan \
			id 5001 remote "$SERVER_IP" local "$CLIENT_IP" dev link0 dstport 4789
	else
		ip -netns "$CLIENT_NS" link add tun0 type geneve \
			id 5001 remote "$SERVER_IP"
	fi
	ip -netns "$CLIENT_NS" link set tun0 up
	ip -netns "$CLIENT_NS" addr replace "$CLIENT_IP4_TUN/24" dev tun0
	ip -netns "$CLIENT_NS" addr replace "$CLIENT_IP6_TUN/112" dev tun0 nodad
	ip -netns "$CLIENT_NS" link set tun0 \
		gso_max_size 196608 gso_ipv4_max_size 196608 \
		gro_max_size 196608 gro_ipv4_max_size 196608
	if [ "$1" = vxlan ]; then
		ip -netns "$SERVER_NS" link add tun1 type vxlan \
			id 5001 remote "$CLIENT_IP" local "$SERVER_IP" dev link1 dstport 4789
	else
		ip -netns "$SERVER_NS" link add tun1 type geneve \
			id 5001 remote "$CLIENT_IP"
	fi
	ip -netns "$SERVER_NS" link set tun1 up
	ip -netns "$SERVER_NS" addr replace "$SERVER_IP4_TUN/24" dev tun1
	ip -netns "$SERVER_NS" addr replace "$SERVER_IP6_TUN/112" dev tun1 nodad
	ip -netns "$SERVER_NS" link set tun1 \
		gso_max_size 196608 gso_ipv4_max_size 196608 \
		gro_max_size 196608 gro_ipv4_max_size 196608

	ip netns exec "$CLIENT_NS" ethtool -K link0 tx-checksumming "$3" > /dev/null
	ip netns exec "$SERVER_NS" ethtool -K link1 tx-checksumming "$3" > /dev/null
}

cleanup_tunnel() {
	ip -netns "$CLIENT_NS" link del tun0
	ip -netns "$SERVER_NS" link del tun1
}

cleanup() {
	ip netns pids "$SERVER_NS" | xargs -r kill
	ip netns pids "$CLIENT_NS" | xargs -r kill
	ip netns del "$SERVER_NS"
	ip netns del "$CLIENT_NS"
	rm -rf "$WORKDIR"
}

do_test() {
	# When tx csum offload is off, software GSO is performed before passing the
	# packet to veth. Check BIG TCP packets inside the VXLAN tunnel to verify
	# the software checksum path: if the checksum code is broken, these packets
	# will be dropped.
	if [ "$2" = on ]; then
		CAPTURE_IFACE='link'
	else
		CAPTURE_IFACE='tun'
	fi

	ip netns exec "$SERVER_NS" tcpdump -nn -s 256 -i "${CAPTURE_IFACE}1" greater 65536 -w "$WORKDIR/server.pcap" 2> /dev/null &
	TCPDUMP_SERVER_PID="$!"
	ip netns exec "$CLIENT_NS" tcpdump -nn -s 256 -i "${CAPTURE_IFACE}0" greater 65536 -w "$WORKDIR/client.pcap" 2> /dev/null &
	TCPDUMP_CLIENT_PID="$!"

	# This filter doesn't capture all possible variants of SACK, but it's aimed
	# at the typical one where SACK follows after [nop, nop, timestamp, nop,
	# nop] (14 bytes after the 20-byte TCP header). IPv6 needs a separate match,
	# because man tcpdump says:
	# > Arithmetic expression against transport layer headers, like tcp[0], does
	# > not work against IPv6 packets.  It only looks at IPv4 packets.
	ip netns exec "$SERVER_NS" tcpdump -nn -s 256 -i "tun1" '(tcp[tcpflags] & (tcp-syn|tcp-ack) = tcp-ack and tcp[34:2] & 0xffc3 = 0x0502) or (ip6[6] = 0x06 and ip6[53] & 0x12 = 0x10 and ip6[74:2] & 0xffc3 = 0x0502)' -w "$WORKDIR/sack.pcap" 2> /dev/null &
	TCPDUMP_SACK_PID="$!"

	if [ "$1" = 4 ]; then
		SERVER_IP="$SERVER_IP4_TUN"
		echo "Running IPv4 traffic in the tunnel"
	else
		SERVER_IP="$SERVER_IP6_TUN"
		echo "Running IPv6 traffic in the tunnel"
	fi

	sleep 1 # Give tcpdump a second to spin up.
	ip netns exec "$CLIENT_NS" netperf -t TCP_STREAM -l 5 -H "$SERVER_IP" -- \
		-m 80000 > /dev/null
	sleep 1 # Give tcpdump a second to process buffered packets.
	kill "$TCPDUMP_SERVER_PID" "$TCPDUMP_CLIENT_PID" "$TCPDUMP_SACK_PID"
	wait "$TCPDUMP_SERVER_PID" "$TCPDUMP_CLIENT_PID" "$TCPDUMP_SACK_PID"
	PACKETS_SERVER=$(tcpdump --count -r "$WORKDIR/server.pcap" 2> /dev/null | cut -d ' ' -f 1)
	PACKETS_CLIENT=$(tcpdump --count -r "$WORKDIR/client.pcap" 2> /dev/null | cut -d ' ' -f 1)
	PACKETS_SACK=$(tcpdump --count -r "$WORKDIR/sack.pcap" 2> /dev/null | cut -d ' ' -f 1)

	echo "Captured BIG TCP RX packets: $PACKETS_SERVER"
	echo "Captured BIG TCP TX packets: $PACKETS_CLIENT"
	echo "Captured TCP SACK packets: $PACKETS_SACK"
	[ "$PACKETS_SERVER" -gt "$PACKETS_THRESHOLD" ] || return 1
	[ "$PACKETS_CLIENT" -gt "$PACKETS_THRESHOLD" ] || return 1
	[ "$PACKETS_SACK" -lt "$(( PACKETS_CLIENT / 2 ))" ] || return 1
}

if ! netperf -V &> /dev/null; then
	echo "SKIP: Could not run test without netperf tool"
	exit "$ksft_skip"
fi

if ! tcpdump --version &> /dev/null; then
	echo "SKIP: Could not run test without tcpdump tool"
	exit "$ksft_skip"
fi

if ! ethtool --version &> /dev/null; then
	echo "SKIP: Could not run test without ethtool tool"
	exit "$ksft_skip"
fi

if ! ip link help 2>&1 | grep gso_ipv4_max_size &> /dev/null; then
	echo "SKIP: Could not run test without gso/gro_ipv4_max_size supported in ip-link"
	exit "$ksft_skip"
fi

WORKDIR=$(mktemp -d)
trap cleanup EXIT
setup
for tunnel in vxlan geneve; do
	for tun_family in 4 6; do
		for traffic_family in 4 6; do
			for csum_offload in on off; do
				setup_tunnel "$tunnel" "$tun_family" "$csum_offload" || exit "$?"
				do_test "$traffic_family" "$csum_offload" || exit "$?"
				cleanup_tunnel
			done
		done
	done
done
