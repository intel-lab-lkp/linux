#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test ECMP path re-selection on TCP retransmission timeout.
#
# Two namespaces connected by two parallel veth pairs with a 2-way ECMP
# route.  When a TCP path is blocked (via tc drop), RTO triggers
# sk_rethink_txhash() + sk_dst_reset(), causing the next route lookup
# to select the other ECMP path.
#
# False negative: ~(1/2)^25 ≈ 3e-8.  With tcp_syn_retries=6 (~127 s
# timeout) and tcp_syn_linear_timeouts=20 there are roughly 26
# independent rehash attempts, each choosing one of 2 paths uniformly.

source lib.sh

SUBNETS=(a b)
PORT=9900

ALL_TESTS="
	test_ecmp_rto_rehash
	test_ecmp_synack_rehash
	test_ecmp_midstream_rehash
"

link_tx_packets_get()
{
	local ns=$1; shift
	local dev=$1; shift

	ip netns exec "$ns" cat "/sys/class/net/$dev/statistics/tx_packets"
}

# Return the number of packets matched by the tc filter action on a device.
# When tc drops packets via "action drop", the device's tx_packets is not
# incremented (packet never reaches veth_xmit), but the tc action maintains
# its own counter.
tc_filter_pkt_count()
{
	local ns=$1; shift
	local dev=$1; shift

	ip netns exec "$ns" tc -s filter show dev "$dev" parent 1: 2>/dev/null |
		awk '/Sent .* pkt/ { for (i=1;i<=NF;i++) if ($i=="pkt") { print $(i-1); exit } }'
}

# Read TcpTimeoutRehash counter from /proc/net/netstat in a namespace.
# This counter increments in tcp_write_timeout() on every RTO that triggers
# sk_rethink_txhash().
get_timeout_rehash_count()
{
	local ns=$1; shift

	ip netns exec "$ns" awk '
		/^TcpExt:/ {
			if (!h) { split($0, n); h=1 }
			else {
				split($0, v)
				for (i in n)
					if (n[i] == "TcpTimeoutRehash") print v[i]
			}
		}
	' /proc/net/netstat
}

# Block TCP (IPv6 next-header = 6) egress, allowing ICMPv6 through.
block_tcp()
{
	local ns=$1; shift
	local dev=$1; shift

	ip netns exec "$ns" tc qdisc add dev "$dev" root handle 1: prio
	ip netns exec "$ns" tc filter add dev "$dev" parent 1: \
		protocol ipv6 prio 1 u32 match u8 0x06 0xff at 6 action drop
}

unblock_tcp()
{
	local ns=$1; shift
	local dev=$1; shift

	ip netns exec "$ns" tc qdisc del dev "$dev" root 2>/dev/null
}

# Return success when both devices have dropped at least one TCP packet.
both_devs_attempted()
{
	local ns=$1; shift
	local dev0=$1; shift
	local dev1=$1; shift

	local c0 c1
	c0=$(tc_filter_pkt_count "$ns" "$dev0")
	c1=$(tc_filter_pkt_count "$ns" "$dev1")
	[ "${c0:-0}" -ge 1 ] && [ "${c1:-0}" -ge 1 ]
}

setup()
{
	setup_ns NS1 NS2

	local ns
	for ns in "$NS1" "$NS2"; do
		ip netns exec "$ns" sysctl -qw net.ipv6.conf.all.accept_dad=0
		ip netns exec "$ns" sysctl -qw net.ipv6.conf.default.accept_dad=0
		ip netns exec "$ns" sysctl -qw net.ipv6.conf.all.forwarding=1
		ip netns exec "$ns" sysctl -qw net.core.txrehash=1
	done

	local i sub
	for i in 0 1; do
		sub=${SUBNETS[$i]}
		ip link add "veth${i}a" type veth peer name "veth${i}b"
		ip link set "veth${i}a" netns "$NS1"
		ip link set "veth${i}b" netns "$NS2"
		ip -n "$NS1" addr add "fd00:${sub}::1/64" dev "veth${i}a"
		ip -n "$NS2" addr add "fd00:${sub}::2/64" dev "veth${i}b"
		ip -n "$NS1" link set "veth${i}a" up
		ip -n "$NS2" link set "veth${i}b" up
	done

	ip -n "$NS1" addr add fd00:ff::1/128 dev lo
	ip -n "$NS2" addr add fd00:ff::2/128 dev lo

	# Allow many SYN retries at 1-second intervals (linear, no
	# exponential backoff) so the rehash test has enough attempts
	# to exercise both ECMP paths deterministically.
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_syn_retries=6
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_syn_linear_timeouts=20

	ip -n "$NS1" -6 route add fd00:ff::2/128 \
		nexthop via fd00:a::2 dev veth0a \
		nexthop via fd00:b::2 dev veth1a

	ip -n "$NS2" -6 route add fd00:ff::1/128 \
		nexthop via fd00:a::1 dev veth0b \
		nexthop via fd00:b::1 dev veth1b

	for i in 0 1; do
		sub=${SUBNETS[$i]}
		ip netns exec "$NS1" \
			ping -6 -c1 -W5 "fd00:${sub}::2" &>/dev/null
		ip netns exec "$NS2" \
			ping -6 -c1 -W5 "fd00:${sub}::1" &>/dev/null
	done

	if ! ip netns exec "$NS1" ping -6 -c1 -W5 fd00:ff::2 &>/dev/null; then
		echo "Basic connectivity check failed"
		return $ksft_skip
	fi
}

# Block ALL paths, start a connection, wait until SYNs have been dropped
# on both interfaces (proving rehash steered the SYN to a new path), then
# unblock so the connection completes.
test_ecmp_rto_rehash()
{
	RET=0

	block_tcp "$NS1" veth0a
	defer unblock_tcp "$NS1" veth0a
	block_tcp "$NS1" veth1a
	defer unblock_tcp "$NS1" veth1a

	ip netns exec "$NS2" socat \
		"TCP6-LISTEN:$PORT,bind=[fd00:ff::2],reuseaddr,fork" \
		EXEC:"echo ESTABLISH_OK" &
	defer kill_process $!

	wait_local_port_listen "$NS2" $PORT tcp

	local rehash_before
	rehash_before=$(get_timeout_rehash_count "$NS1")

	# Start the connection in the background; it will retry SYNs at
	# 1-second intervals until an unblocked path is found.
	ip netns exec "$NS1" bash -c \
		"echo test | socat - \
		'TCP6:[fd00:ff::2]:$PORT,bind=[fd00:ff::1],connect-timeout=60'" \
		>"/tmp/ecmp_rto_$$" 2>&1 &
	local client_pid=$!
	defer kill_process $client_pid

	# Wait until both paths have seen at least one dropped SYN.
	# This proves sk_rethink_txhash() rehashed the connection from
	# one ECMP path to the other.
	slowwait 30 both_devs_attempted "$NS1" veth0a veth1a
	check_err $? "SYNs did not appear on both paths (rehash not working)"
	if [ $RET -ne 0 ]; then
		log_test "ECMP RTO rehash: establish with blocked paths"
		return
	fi

	# Unblock both paths and let the next SYN retransmit succeed.
	unblock_tcp "$NS1" veth0a
	unblock_tcp "$NS1" veth1a

	local rc=0
	wait $client_pid || rc=$?

	local result
	result=$(cat "/tmp/ecmp_rto_$$" 2>/dev/null)
	rm -f "/tmp/ecmp_rto_$$"

	if [ $rc -ne 0 ] || [[ "$result" != *"ESTABLISH_OK"* ]]; then
		check_err 1 "connection failed after unblocking: $result"
	fi

	local rehash_after
	rehash_after=$(get_timeout_rehash_count "$NS1")
	if [ "$rehash_after" -le "$rehash_before" ]; then
		check_err 1 "TcpTimeoutRehash counter did not increment"
	fi

	log_test "ECMP RTO rehash: establish with blocked paths"
}

# Block the server's return paths so SYN/ACKs are dropped.  The client
# retransmits SYNs at 1-second intervals; each duplicate SYN arriving at
# the server updates ir_iif to match the new arrival interface, so the
# retransmitted SYN/ACK routes back via the interface the SYN arrived on.
test_ecmp_synack_rehash()
{
	RET=0
	local port=$((PORT + 2))

	block_tcp "$NS2" veth0b
	defer unblock_tcp "$NS2" veth0b
	block_tcp "$NS2" veth1b
	defer unblock_tcp "$NS2" veth1b

	ip netns exec "$NS2" socat \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr,fork" \
		EXEC:"echo SYNACK_OK" &
	defer kill_process $!

	wait_local_port_listen "$NS2" $port tcp

	# Start the connection; SYNs reach the server (client egress is
	# open) but SYN/ACKs are dropped on the server's return path.
	ip netns exec "$NS1" bash -c \
		"echo test | socat - \
		'TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1],connect-timeout=60'" \
		>"/tmp/ecmp_synack_$$" 2>&1 &
	local client_pid=$!
	defer kill_process $client_pid

	# Wait until both server-side interfaces have dropped at least
	# one SYN/ACK, proving the server rehashed its return path.
	slowwait 30 both_devs_attempted "$NS2" veth0b veth1b
	check_err $? "SYN/ACKs did not appear on both return paths"
	if [ $RET -ne 0 ]; then
		log_test "ECMP SYN/ACK rehash: blocked return path"
		return
	fi

	# Unblock and let the connection complete.
	unblock_tcp "$NS2" veth0b
	unblock_tcp "$NS2" veth1b

	local rc=0
	wait $client_pid || rc=$?

	local result
	result=$(cat "/tmp/ecmp_synack_$$" 2>/dev/null)
	rm -f "/tmp/ecmp_synack_$$"

	if [ $rc -ne 0 ] || [[ "$result" != *"SYNACK_OK"* ]]; then
		check_err 1 "connection failed after unblocking: $result"
	fi

	log_test "ECMP SYN/ACK rehash: blocked return path"
}

# Establish a data transfer with both paths open, then block the
# active path.  Verify the transfer continues via rehash and that
# TcpTimeoutRehash incremented.
test_ecmp_midstream_rehash()
{
	RET=0
	local port=$((PORT + 1))

	ip netns exec "$NS2" socat -u \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" - >/dev/null &
	defer kill_process $!

	wait_local_port_listen "$NS2" $port tcp

	local base_tx0 base_tx1
	base_tx0=$(link_tx_packets_get "$NS1" veth0a)
	base_tx1=$(link_tx_packets_get "$NS1" veth1a)

	ip netns exec "$NS1" bash -c "
		for i in \$(seq 1 40); do
			dd if=/dev/zero bs=10k count=1 2>/dev/null
			sleep 0.25
		done | timeout 60 socat - 'TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1]'
	" &>/dev/null &
	local client_pid=$!
	defer kill_process $client_pid

	busywait $BUSYWAIT_TIMEOUT until_counter_is \
			">= $((base_tx0 + base_tx1 + 20))" \
		link_tx_packets_total "$NS1"
	check_err $? "no TX activity detected"
	if [ $RET -ne 0 ]; then
		log_test "ECMP midstream rehash: block active path"
		return
	fi

	# Find the active path and block it.
	local cur0 cur1 active_idx
	cur0=$(link_tx_packets_get "$NS1" veth0a)
	cur1=$(link_tx_packets_get "$NS1" veth1a)
	if [ $((cur0 - base_tx0)) -ge $((cur1 - base_tx1)) ]; then
		active_idx=0
	else
		active_idx=1
	fi

	local rehash_before
	rehash_before=$(get_timeout_rehash_count "$NS1")

	block_tcp "$NS1" "veth${active_idx}a"
	defer unblock_tcp "$NS1" "veth${active_idx}a"

	local rc=0
	wait $client_pid || rc=$?

	check_err $rc "data transfer failed after blocking veth${active_idx}a"

	local rehash_after
	rehash_after=$(get_timeout_rehash_count "$NS1")
	if [ "$rehash_after" -le "$rehash_before" ]; then
		check_err 1 "TcpTimeoutRehash counter did not increment"
	fi

	log_test "ECMP midstream rehash: block active path"
}

link_tx_packets_total()
{
	local ns=$1; shift

	echo $(( $(link_tx_packets_get "$ns" veth0a) +
		 $(link_tx_packets_get "$ns" veth1a) ))
}

require_command socat

trap cleanup_all_ns EXIT
setup || exit $?
tests_run
exit $EXIT_STATUS
