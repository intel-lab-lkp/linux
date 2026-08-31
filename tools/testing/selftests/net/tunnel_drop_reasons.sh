#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test the drop reasons reported by the generic tunnel RX path,
# ip_tunnel_rcv() and __ip6_tnl_rcv().
#
# Two situations are checked, for both GRE and ip6gre:
#
#  - the options carried by the packet do not match the tunnel
#    configuration, which is reported as IP_TUNNEL_CFG_OPTS_MISMATCH.
#    It is triggered here by configuring the receiver with 'iseq' or
#    'icsum' while the sender emits neither.
#
#  - the sequence number of the packet is older than the one expected by
#    the tunnel, which is reported as IP_TUNNEL_OLD_SEQ.  It is
#    triggered here by recreating the tunnel device on the sender, which
#    resets its outgoing sequence number the same way a peer reboot
#    would.
#
# A control case, where both endpoints agree on the options, makes sure
# that no tunnel drop reason is reported when packets are accepted.
#
# The GRE specific reasons are checked as well:
#
#  - a packet that matches no tunnel is reported as
#    GRE_TUNNEL_NOT_FOUND.  It is triggered here by giving the two
#    endpoints different keys.
#
# On the transmit side, the reasons reported while encapsulating are
# checked too: a packet that does not fit the path MTU, a device in
# external mode that receives no metadata, and a tunnel whose remote
# endpoint has no route.
#
#  - a header with the routing bit set is reported as GRE_INVALID_HDR,
#    and a header announcing a GRE version nobody handles is reported as
#    UNHANDLED_PROTO.  Both are triggered by corrupting the GRE header
#    on ingress with tc pedit, and are skipped if that is not available.
#
# Drop reasons are read from the skb:kfree_skb tracepoint.  A dedicated
# trace instance is used so that the test does not disturb, and is not
# disturbed by, anything else using the tracing facility.

source lib.sh

NS_SND=""
NS_RCV=""
TRACE_DIR=""
TR=""

SND_V4=10.0.0.1
RCV_V4=10.0.0.2
SND_V6=2001:db8::1
RCV_V6=2001:db8::2
TUN_SND=192.168.1.1
TUN_RCV=192.168.1.2

cleanup()
{
	if [ -n "$TR" ]; then
		echo 0 > "$TR/events/skb/kfree_skb/enable" 2>/dev/null
		rmdir "$TR" 2>/dev/null
	fi
	cleanup_all_ns
}

trap cleanup EXIT

setup_tracing()
{
	local dir

	for dir in /sys/kernel/tracing /sys/kernel/debug/tracing; do
		if [ -f "$dir/trace" ]; then
			TRACE_DIR="$dir"
			break
		fi
	done
	[ -n "$TRACE_DIR" ] || return 1
	[ -d "$TRACE_DIR/instances" ] || return 1
	[ -e "$TRACE_DIR/events/skb/kfree_skb" ] || return 1

	TR="$TRACE_DIR/instances/tunnel_drop_reasons"
	mkdir "$TR" 2>/dev/null || return 1
	echo 1 > "$TR/events/skb/kfree_skb/enable" || return 1
}

setup_ns_pair()
{
	PING_ARGS=""
	cleanup_all_ns
	setup_ns NS_SND NS_RCV

	ip link add veth_s netns "$NS_SND" type veth \
		peer name veth_r netns "$NS_RCV"
	ip -n "$NS_SND" link set veth_s up
	ip -n "$NS_RCV" link set veth_r up

	ip -n "$NS_SND" addr add "$SND_V4/24" dev veth_s
	ip -n "$NS_RCV" addr add "$RCV_V4/24" dev veth_r
	ip -n "$NS_SND" addr add "$SND_V6/64" dev veth_s nodad
	ip -n "$NS_RCV" addr add "$RCV_V6/64" dev veth_r nodad
}

# $1: namespace, $2: local, $3: remote, $4...: tunnel options
add_gre()
{
	local ns=$1 loc=$2 rem=$3

	shift 3
	ip -n "$ns" link add gre_test type gre local "$loc" remote "$rem" "$@"
	ip -n "$ns" link set gre_test up
}

# $1: namespace, $2: local, $3: remote, $4...: tunnel options
add_ip6gre()
{
	local ns=$1 loc=$2 rem=$3

	shift 3
	ip -n "$ns" link add gre_test type ip6gre local "$loc" remote "$rem" \
		"$@"
	ip -n "$ns" link set gre_test up
}

addr_tunnels()
{
	ip -n "$NS_SND" addr add "$TUN_SND/24" dev gre_test
	ip -n "$NS_RCV" addr add "$TUN_RCV/24" dev gre_test
}

# Traffic used by check_reason().  Tests that need something else set
# PING_ARGS before calling it.
PING_ARGS=""

send_traffic()
{
	# shellcheck disable=SC2086
	ip netns exec "$NS_SND" ping -c 2 -W 1 $PING_ARGS "$TUN_RCV" \
		>/dev/null 2>&1
	# Let the tracepoint records reach the trace buffer.
	sleep 1
}

# $1: test name, $2: expected reason, empty if none is expected
check_reason()
{
	local name=$1 want=$2 count

	echo > "$TR/trace"
	send_traffic

	if [ -n "$want" ]; then
		count=$(grep -c "reason: $want" "$TR/trace")
		if [ "$count" -gt 0 ]; then
			RET=$ksft_pass
		else
			RET=$ksft_fail
		fi
		log_test "$name" "$count dropped"
	else
		count=$(grep -c "reason: IP_TUNNEL_" "$TR/trace")
		if [ "$count" -eq 0 ]; then
			RET=$ksft_pass
		else
			RET=$ksft_fail
		fi
		log_test "$name" "$count dropped"
	fi
}

test_opts_mismatch()
{
	local proto=$1 opt=$2
	local add=add_gre loc=$SND_V4 rem=$RCV_V4

	if [ "$proto" = "ip6gre" ]; then
		add=add_ip6gre
		loc=$SND_V6
		rem=$RCV_V6
	fi

	setup_ns_pair
	# The sender emits no option, the receiver expects one.
	$add "$NS_SND" "$loc" "$rem"
	$add "$NS_RCV" "$rem" "$loc" "$opt"
	addr_tunnels

	check_reason "$proto: $opt option mismatch" \
		IP_TUNNEL_CFG_OPTS_MISMATCH
}

test_old_seq()
{
	local proto=$1
	local add=add_gre loc=$SND_V4 rem=$RCV_V4

	if [ "$proto" = "ip6gre" ]; then
		add=add_ip6gre
		loc=$SND_V6
		rem=$RCV_V6
	fi

	setup_ns_pair
	$add "$NS_SND" "$loc" "$rem" oseq
	$add "$NS_RCV" "$rem" "$loc" iseq
	addr_tunnels

	# Raise the sequence number expected by the receiver, then reset the
	# one used by the sender, as a peer reboot would do.
	send_traffic
	ip -n "$NS_SND" link del gre_test
	$add "$NS_SND" "$loc" "$rem" oseq
	ip -n "$NS_SND" addr add "$TUN_SND/24" dev gre_test

	check_reason "$proto: old sequence number" IP_TUNNEL_OLD_SEQ
}

test_control()
{
	setup_ns_pair
	add_gre "$NS_SND" "$SND_V4" "$RCV_V4" oseq ocsum
	add_gre "$NS_RCV" "$RCV_V4" "$SND_V4" iseq icsum
	addr_tunnels

	check_reason "gre: matching configuration (control)" ""
}

# Corrupt one field of the GRE header of every IPv4 packet received by
# the receiver.  $1 is a tc pedit munge expression, with offsets counted
# from the start of the IPv4 header.
corrupt_gre_header()
{
	ip netns exec "$NS_RCV" tc qdisc add dev veth_r ingress || return 1
	ip netns exec "$NS_RCV" tc filter add dev veth_r ingress \
		protocol ip matchall action pedit ex munge "$@" || return 1
}

test_tunnel_not_found()
{
	setup_ns_pair
	# The two endpoints use different keys, so the lookup on the
	# receiver finds no tunnel for the incoming packets.
	add_gre "$NS_SND" "$SND_V4" "$RCV_V4" okey 1 ikey 1
	add_gre "$NS_RCV" "$RCV_V4" "$SND_V4" okey 2 ikey 2
	addr_tunnels

	check_reason "gre: tunnel not found" GRE_TUNNEL_NOT_FOUND
}

# $1: test name, $2: expected reason, $3...: tc pedit munge expression
test_corrupted_header()
{
	local name=$1 want=$2

	shift 2
	setup_ns_pair
	add_gre "$NS_SND" "$SND_V4" "$RCV_V4"
	add_gre "$NS_RCV" "$RCV_V4" "$SND_V4"
	addr_tunnels

	if ! corrupt_gre_header "$@" 2>/dev/null; then
		log_test_skip "$name"
		return
	fi

	check_reason "$name" "$want"
}

test_tx_pkt_too_big()
{
	setup_ns_pair
	# The underlay cannot carry what the tunnel advertises, so the
	# encapsulated packet does not fit the path MTU.  The kernel sends
	# an ICMP fragmentation needed back and drops it, which is path MTU
	# discovery working as intended.
	ip -n "$NS_SND" link set veth_s mtu 1280
	ip -n "$NS_RCV" link set veth_r mtu 1280
	add_gre "$NS_SND" "$SND_V4" "$RCV_V4"
	add_gre "$NS_RCV" "$RCV_V4" "$SND_V4"
	ip -n "$NS_SND" link set gre_test mtu 1500
	addr_tunnels

	PING_ARGS="-s 1400 -M do"
	check_reason "gre: packet does not fit the path MTU" PKT_TOO_BIG
}

test_tx_no_metadata()
{
	setup_ns_pair
	# A device in external mode expects the destination to come from
	# the tunnel metadata, which plain traffic does not carry.
	ip -n "$NS_SND" link add gre_test type gre external
	ip -n "$NS_SND" link set gre_test up
	ip -n "$NS_SND" addr add "$TUN_SND/24" dev gre_test

	check_reason "gre: external mode without metadata" TUNNEL_TXINFO
}

test_tx_no_route()
{
	setup_ns_pair
	# Nothing knows how to reach the remote endpoint of the tunnel.
	add_gre "$NS_SND" "$SND_V4" 172.31.255.254
	ip -n "$NS_SND" addr add "$TUN_SND/24" dev gre_test

	check_reason "gre: no route to the remote endpoint" IP_OUTNOROUTES
}

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: need root"
	exit "$ksft_skip"
fi

if ! setup_tracing; then
	echo "SKIP: could not set up a trace instance for skb:kfree_skb"
	exit "$ksft_skip"
fi

test_opts_mismatch gre iseq
test_opts_mismatch gre icsum
test_control
test_old_seq gre

test_tunnel_not_found
# The routing bit is the second most significant bit of the first byte
# of the GRE header, which follows the 20 byte IPv4 header.
test_corrupted_header "gre: routing bit set" GRE_INVALID_HDR \
	offset 20 u8 set 0x40
# The GRE version sits in the low bits of the next byte.  Version 1 is
# PPTP, which has no handler here.
test_corrupted_header "gre: unhandled GRE version" UNHANDLED_PROTO \
	offset 21 u8 set 0x01

test_tx_pkt_too_big
test_tx_no_metadata
test_tx_no_route

if [ -e /proc/sys/net/ipv6 ]; then
	test_opts_mismatch ip6gre iseq
	test_old_seq ip6gre
else
	log_test_skip "ip6gre: iseq option mismatch"
	log_test_skip "ip6gre: old sequence number"
fi

exit "$EXIT_STATUS"
