#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test local ECMP path re-selection on TCP retransmission timeout and PLB.
#
# Two namespaces connected by two parallel veth pairs with a 2-way ECMP
# route.  When a TCP path is blocked (via tc drop) or congested (via
# netem ECN marking), the kernel rehashes the connection via
# sk_rethink_txhash() + __sk_dst_reset(), causing the next route lookup
# to select the other ECMP path.

source lib.sh

SUBNETS=(a b)
PORT=9900
: "${ECMP_REBUILD_ROUNDS:=10}"

ALL_TESTS="
	test_ecmp_syn_rehash
	test_ecmp_synack_rehash
	test_ecmp_midstream_rehash
	test_ecmp_midstream_ack_rehash
	test_ecmp_plb_rehash
	test_ecmp_hash_policy1_no_rehash
	test_ecmp_no_flowlabel_leak
	test_ecmp_dst_rebuild_consistency
	test_ecmp_dst_rebuild_syncookie_consistency
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
		awk '/Sent .* pkt/ {
			for (i=1; i<=NF; i++)
				if ($i == "pkt") { print $(i-1); exit }
		}'
}

# Read a TcpExt counter from /proc/net/netstat in a namespace.
# Returns 0 if the counter is not found.
get_netstat_counter()
{
	local ns=$1; shift
	local field=$1; shift
	local val

	# shellcheck disable=SC2016
	val=$(ip netns exec "$ns" awk -v key="$field" '
		/^TcpExt:/ {
			if (!h) { split($0, n); h=1 }
			else {
				split($0, v)
				for (i in n)
					if (n[i] == key) print v[i]
			}
		}
	' /proc/net/netstat)
	echo "${val:-0}"
}

# Apply netem ECN marking: CE-mark all ECT packets instead of dropping them.
mark_ecn()
{
	local ns=$1; shift
	local dev=$1; shift

	ip netns exec "$ns" tc qdisc add dev "$dev" root netem loss 100% ecn
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

# Return success when a device's TX counter exceeds a baseline value.
dev_tx_packets_above()
{
	local ns=$1; shift
	local dev=$1; shift
	local baseline=$1; shift

	local cur
	cur=$(link_tx_packets_get "$ns" "$dev")
	[ "$cur" -gt "$baseline" ]
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

link_tx_packets_total()
{
	local ns=$1; shift

	echo $(( $(link_tx_packets_get "$ns" veth0a) +
		 $(link_tx_packets_get "$ns" veth1a) ))
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
	# to exercise both ECMP paths.
	if ! ip netns exec "$NS1" sysctl -qw \
	     net.ipv4.tcp_syn_linear_timeouts=25; then
		echo "SKIP: tcp_syn_linear_timeouts not supported"
		return "$ksft_skip"
	fi
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_syn_retries=25

	# Keep the server's request socket alive during the blocking
	# period so SYN/ACK retransmits continue.
	ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_synack_retries=25

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
		return "$ksft_skip"
	fi
}

# Block ALL paths, start a connection, wait until SYNs have been dropped
# on both interfaces (proving rehash steered the SYN to a new path), then
# unblock so the connection completes.
test_ecmp_syn_rehash()
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

	wait_local_port_listen "$NS2" "$PORT" tcp

	local rehash_before
	rehash_before=$(get_netstat_counter "$NS1" TcpTimeoutRehash)

	# Start the connection in the background; it will retry SYNs at
	# 1-second intervals until an unblocked path is found.
	# Use -u (unidirectional) to only receive from the server;
	# sending data back would risk SIGPIPE if the server's EXEC
	# child has already exited.
	local tmpfile
	tmpfile=$(mktemp)
	defer rm -f "$tmpfile"

	ip netns exec "$NS1" socat -u \
		"TCP6:[fd00:ff::2]:$PORT,bind=[fd00:ff::1],connect-timeout=60" \
		STDOUT >"$tmpfile" 2>&1 &
	local client_pid=$!
	defer kill_process "$client_pid"

	# Wait until both paths have seen at least one dropped SYN.
	# This proves sk_rethink_txhash() rehashed the connection from
	# one ECMP path to the other.
	slowwait 30 both_devs_attempted "$NS1" veth0a veth1a
	check_err $? "SYNs did not appear on both paths (rehash not working)"
	if [ "$RET" -ne 0 ]; then
		log_test "Local ECMP SYN rehash: establish with blocked paths"
		return
	fi

	# Unblock both paths and let the next SYN retransmit succeed.
	unblock_tcp "$NS1" veth0a
	unblock_tcp "$NS1" veth1a

	local rc=0
	wait "$client_pid" || rc=$?

	local result
	result=$(cat "$tmpfile" 2>/dev/null)

	if [[ "$result" != *"ESTABLISH_OK"* ]]; then
		check_err 1 "connection failed after unblocking (rc=$rc): $result"
	fi

	local rehash_after
	rehash_after=$(get_netstat_counter "$NS1" TcpTimeoutRehash)
	if [ "$rehash_after" -le "$rehash_before" ]; then
		check_err 1 "TcpTimeoutRehash counter did not increment"
	fi

	log_test "Local ECMP SYN rehash: establish with blocked paths"
}

# Block the server's return paths so SYN/ACKs are dropped.  The client
# retransmits SYNs at 1-second intervals; each duplicate SYN arriving at
# the server triggers tcp_rtx_synack() which re-rolls txhash, so the
# retransmitted SYN/ACK selects a different ECMP return path.
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

	wait_local_port_listen "$NS2" "$port" tcp

	# Start the connection; SYNs reach the server (client egress is
	# open) but SYN/ACKs are dropped on the server's return path.
	local tmpfile
	tmpfile=$(mktemp)
	defer rm -f "$tmpfile"

	ip netns exec "$NS1" socat -u \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1],connect-timeout=60" \
		STDOUT >"$tmpfile" 2>&1 &
	local client_pid=$!
	defer kill_process "$client_pid"

	# Wait until both server-side interfaces have dropped at least
	# one SYN/ACK, proving the server rehashed its return path.
	slowwait 30 both_devs_attempted "$NS2" veth0b veth1b
	check_err $? "SYN/ACKs did not appear on both return paths"
	if [ "$RET" -ne 0 ]; then
		log_test "Local ECMP SYN/ACK rehash: blocked return path"
		return
	fi

	# Unblock and let the connection complete.
	unblock_tcp "$NS2" veth0b
	unblock_tcp "$NS2" veth1b

	local rc=0
	wait "$client_pid" || rc=$?

	local result
	result=$(cat "$tmpfile" 2>/dev/null)

	if [[ "$result" != *"SYNACK_OK"* ]]; then
		check_err 1 "connection failed after unblocking (rc=$rc): $result"
	fi

	log_test "Local ECMP SYN/ACK rehash: blocked return path"
}

# Establish a data transfer with both paths open, then block the
# active path.  Verify that data appears on the previously inactive
# path (proving RTO triggered a rehash) and that TcpTimeoutRehash
# incremented.
#
# With 2-way ECMP each rehash may pick the same path, so a single
# attempt can occasionally fail.  Retry once for robustness.

# Single attempt at the midstream rehash check.  Returns 0 on success.
ecmp_midstream_rehash_attempt()
{
	local port=$1; shift

	ip netns exec "$NS2" socat -u \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" - >/dev/null &
	local server_pid=$!

	wait_local_port_listen "$NS2" "$port" tcp

	local base_tx0 base_tx1
	base_tx0=$(link_tx_packets_get "$NS1" veth0a)
	base_tx1=$(link_tx_packets_get "$NS1" veth1a)

	# Continuous data source; timeout caps overall test duration and
	# must exceed the slowwait below so data keeps flowing.
	ip netns exec "$NS1" timeout 90 socat -u \
		OPEN:/dev/zero \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1]" &>/dev/null &
	local client_pid=$!

	# Wait for enough packets to identify the active path.
	if ! busywait "$BUSYWAIT_TIMEOUT" until_counter_is \
			">= $((base_tx0 + base_tx1 + 10))" \
		link_tx_packets_total "$NS1" > /dev/null; then
		kill "$client_pid" "$server_pid" 2>/dev/null
		wait "$client_pid" "$server_pid" 2>/dev/null
		return 1
	fi

	# Find the active path and block it.
	local current_tx0 current_tx1 active_idx inactive_idx
	current_tx0=$(link_tx_packets_get "$NS1" veth0a)
	current_tx1=$(link_tx_packets_get "$NS1" veth1a)
	if [ $((current_tx0 - base_tx0)) -ge $((current_tx1 - base_tx1)) ]; then
		active_idx=0; inactive_idx=1
	else
		active_idx=1; inactive_idx=0
	fi

	local rehash_before
	rehash_before=$(get_netstat_counter "$NS1" TcpTimeoutRehash)
	# Suppress __dst_negative_advice() in tcp_write_timeout() so
	# that __sk_dst_reset() is the only dst-invalidation mechanism
	# on the RTO path.
	local saved_retries1
	saved_retries1=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_retries1)
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_retries1=255

	block_tcp "$NS1" "veth${active_idx}a"

	# Capture baseline after block_tcp returns.  block_tcp adds a
	# prio qdisc then a tc filter; between those two steps the
	# qdisc's CAN_BYPASS fast-path lets packets through unfiltered.
	local inactive_before
	inactive_before=$(link_tx_packets_get "$NS1" "veth${inactive_idx}a")

	# Wait for meaningful data on the previously inactive path,
	# proving RTO triggered a rehash and data actually moved.
	local rc=0
	if ! slowwait 60 dev_tx_packets_above \
		"$NS1" "veth${inactive_idx}a" "$((inactive_before + 100))"; then
		rc=1
	fi

	local rehash_after
	rehash_after=$(get_netstat_counter "$NS1" TcpTimeoutRehash)
	if [ "$rehash_after" -le "$rehash_before" ]; then
		rc=1
	fi

	unblock_tcp "$NS1" "veth${active_idx}a"
	ip netns exec "$NS1" sysctl -qw \
		net.ipv4.tcp_retries1="$saved_retries1"
	kill "$client_pid" "$server_pid" 2>/dev/null
	wait "$client_pid" "$server_pid" 2>/dev/null
	return "$rc"
}

test_ecmp_midstream_rehash()
{
	RET=0
	local port=$((PORT + 1))

	if ! ecmp_midstream_rehash_attempt "$port"; then
		ecmp_midstream_rehash_attempt "$((port + 1))"
		check_err $? "data did not appear on alternate path after blocking"
	fi

	log_test "Local ECMP midstream rehash: block active path"
}

# Single attempt at the ACK rehash check.  Returns 0 on success.
ecmp_ack_rehash_attempt()
{
	local port=$1; shift

	ip netns exec "$NS2" socat -u \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" - >/dev/null &
	local server_pid=$!

	wait_local_port_listen "$NS2" "$port" tcp

	local base_total
	base_total=$(link_tx_packets_total "$NS1")

	# Continuous data source from NS1 to NS2.
	ip netns exec "$NS1" timeout 120 socat -u \
		OPEN:/dev/zero \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1]" &>/dev/null &
	local client_pid=$!

	# Wait for data to start flowing.
	if ! busywait "$BUSYWAIT_TIMEOUT" until_counter_is \
			">= $((base_total + 10))" \
		link_tx_packets_total "$NS1" > /dev/null; then
		kill "$client_pid" "$server_pid" 2>/dev/null
		wait "$client_pid" "$server_pid" 2>/dev/null
		return 1
	fi

	local rehash_before
	rehash_before=$(get_netstat_counter "$NS2" TcpDuplicateDataRehash)

	# Block both return paths from NS2 so ACKs are dropped.
	# Data from NS1 still arrives (tc filter is on egress).
	block_tcp "$NS2" veth0b
	block_tcp "$NS2" veth1b

	# NS1 will RTO (no ACKs), retransmit with new flowlabel.
	# NS2 detects the flowlabel change via tcp_rcv_spurious_retrans(),
	# rehashes, and NS2's ACKs try a different ECMP return path.
	# Wait until both NS2 interfaces have dropped at least one ACK.
	local rc=0
	if ! slowwait 60 both_devs_attempted "$NS2" veth0b veth1b; then
		rc=1
	fi

	local rehash_after
	rehash_after=$(get_netstat_counter "$NS2" TcpDuplicateDataRehash)
	if [ "$rehash_after" -le "$rehash_before" ]; then
		rc=1
	fi

	unblock_tcp "$NS2" veth0b
	unblock_tcp "$NS2" veth1b
	kill "$client_pid" "$server_pid" 2>/dev/null
	wait "$client_pid" "$server_pid" 2>/dev/null
	return "$rc"
}

# Block the receiver's (NS2) ACK return paths while data flows from
# NS1 to NS2.  The sender (NS1) times out and retransmits with a new
# flowlabel; the receiver detects the changed flowlabel via
# tcp_rcv_spurious_retrans() and rehashes its own txhash so that its
# ACKs try a different ECMP return path.
#
# With 2-way ECMP each rehash may pick the same path, so a single
# attempt can occasionally fail.  Retry once for robustness.
test_ecmp_midstream_ack_rehash()
{
	RET=0
	local port=$((PORT + 3))

	if ! ecmp_ack_rehash_attempt "$port"; then
		ecmp_ack_rehash_attempt "$((port + 1))"
		check_err $? "ACKs did not appear on both return paths"
	fi

	log_test "Local ECMP midstream ACK rehash: blocked return path"
}

# Establish a DCTCP data transfer with PLB enabled, then ECN-mark both
# paths.  Sustained CE marking triggers PLB to call sk_rethink_txhash()
# + __sk_dst_reset(), bouncing the connection between ECMP paths.
# Verify data appears on both paths and that TCPPLBRehash incremented.
test_ecmp_plb_rehash()
{
	RET=0
	local port=$((PORT + 4))

	# DCTCP is a restricted congestion control algorithm.  Add it to
	# tcp_allowed_congestion_control to mark it TCP_CONG_NON_RESTRICTED
	# so test namespaces can set it as their default.  This avoids
	# changing the host's default congestion control algorithm.
	# The write must be from the root namespace; writes from child
	# namespaces do not take effect.
	local saved_allowed
	saved_allowed=$(sysctl -n net.ipv4.tcp_allowed_congestion_control)
	if ! echo "$saved_allowed" | grep -qw dctcp; then
		local was_loaded
		was_loaded=$(grep -cw tcp_dctcp /proc/modules 2>/dev/null)
		modprobe tcp_dctcp 2>/dev/null
		# Unload only if we loaded it (absent before, present now).
		# Built-in modules never appear in /proc/modules.
		if [ "${was_loaded:-0}" -eq 0 ] &&
		   grep -qw tcp_dctcp /proc/modules 2>/dev/null; then
			defer modprobe -r tcp_dctcp 2>/dev/null
		fi
		if ! sysctl -qw net.ipv4.tcp_allowed_congestion_control="$saved_allowed dctcp"; then
			log_test_skip "Local ECMP PLB rehash: DCTCP not available"
			return "$ksft_skip"
		fi
		defer sysctl -qw \
			net.ipv4.tcp_allowed_congestion_control="$saved_allowed"
	fi

	# Save NS1 sysctls before modifying them.
	local saved_ecn1 saved_cc1 saved_plb_enabled saved_plb_rounds
	local saved_plb_thresh saved_plb_suspend
	saved_ecn1=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_ecn)
	saved_cc1=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_congestion_control)
	saved_plb_enabled=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_plb_enabled)
	saved_plb_rounds=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_plb_rehash_rounds)
	saved_plb_thresh=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_plb_cong_thresh)
	saved_plb_suspend=$(ip netns exec "$NS1" sysctl -n net.ipv4.tcp_plb_suspend_rto_sec)

	# Enable ECN and DCTCP with PLB on the sender.
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_ecn=1
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_congestion_control=dctcp
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_enabled=1
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_rehash_rounds=3
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_cong_thresh=1
	ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_suspend_rto_sec=0
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_ecn="$saved_ecn1"
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_congestion_control="$saved_cc1"
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_enabled="$saved_plb_enabled"
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_rehash_rounds="$saved_plb_rounds"
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_cong_thresh="$saved_plb_thresh"
	defer ip netns exec "$NS1" sysctl -qw net.ipv4.tcp_plb_suspend_rto_sec="$saved_plb_suspend"

	# DCTCP sets ECT on the SYN; the receiver must also use DCTCP
	# so that tcp_ca_needs_ecn(listen_sk) accepts the ECN
	# negotiation.
	local saved_ecn2 saved_cc2
	saved_ecn2=$(ip netns exec "$NS2" sysctl -n net.ipv4.tcp_ecn)
	saved_cc2=$(ip netns exec "$NS2" sysctl -n net.ipv4.tcp_congestion_control)
	ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_ecn=1
	ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_congestion_control=dctcp
	defer ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_ecn="$saved_ecn2"
	defer ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_congestion_control="$saved_cc2"

	ip netns exec "$NS2" socat -u \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" - >/dev/null &
	defer kill_process $!

	wait_local_port_listen "$NS2" "$port" tcp

	local base_tx0 base_tx1
	base_tx0=$(link_tx_packets_get "$NS1" veth0a)
	base_tx1=$(link_tx_packets_get "$NS1" veth1a)

	ip netns exec "$NS1" timeout 90 socat -u \
		OPEN:/dev/zero \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1]" &>/dev/null &
	local client_pid=$!
	defer kill_process "$client_pid"

	# Wait for data to start flowing before applying ECN marking.
	busywait "$BUSYWAIT_TIMEOUT" until_counter_is \
			">= $((base_tx0 + base_tx1 + 10))" \
		link_tx_packets_total "$NS1" > /dev/null
	check_err $? "no TX activity detected"
	if [ "$RET" -ne 0 ]; then
		log_test "Local ECMP PLB rehash: ECN-marked path"
		return
	fi

	# Snapshot TX counters and rehash stats before ECN marking.
	local pre_ecn_tx0 pre_ecn_tx1
	pre_ecn_tx0=$(link_tx_packets_get "$NS1" veth0a)
	pre_ecn_tx1=$(link_tx_packets_get "$NS1" veth1a)

	local plb_before rto_before
	plb_before=$(get_netstat_counter "$NS1" TCPPLBRehash)
	rto_before=$(get_netstat_counter "$NS1" TcpTimeoutRehash)

	# CE-mark all data on both paths.  PLB detects sustained
	# congestion and rehashes, bouncing traffic between paths.
	mark_ecn "$NS1" veth0a
	defer unblock_tcp "$NS1" veth0a	# removes the marking rule
	mark_ecn "$NS1" veth1a
	defer unblock_tcp "$NS1" veth1a	# removes the marking rule

	# Wait for meaningful data on both paths, proving PLB rehashed
	# the connection and traffic actually moved.  Require at least
	# 100 packets beyond the baseline to rule out stray control
	# packets (ND, etc.) satisfying the check.
	slowwait 60 dev_tx_packets_above \
		"$NS1" veth0a "$((pre_ecn_tx0 + 100))"
	check_err $? "no data on veth0a after ECN marking"

	slowwait 60 dev_tx_packets_above \
		"$NS1" veth1a "$((pre_ecn_tx1 + 100))"
	check_err $? "no data on veth1a after ECN marking"

	local plb_after rto_after
	plb_after=$(get_netstat_counter "$NS1" TCPPLBRehash)
	rto_after=$(get_netstat_counter "$NS1" TcpTimeoutRehash)
	if [ "$plb_after" -le "$plb_before" ]; then
		check_err 1 "TCPPLBRehash counter did not increment"
	fi
	if [ "$rto_after" -gt "$rto_before" ]; then
		check_err 1 "TcpTimeoutRehash incremented; rehash was RTO-driven, not PLB"
	fi

	log_test "Local ECMP PLB rehash: ECN-marked path"
}

# Verify that hash policy 1 (L3+L4 symmetric) preserves the ECMP path
# across rehash.  Policy 1 computes a deterministic hash from the
# 5-tuple, so mp_hash stays 0 and rt6_multipath_hash() always selects
# the same path regardless of txhash changes.
test_ecmp_hash_policy1_no_rehash()
{
	RET=0
	local port=$((PORT + 5))

	local saved_policy
	saved_policy=$(ip netns exec "$NS1" sysctl -n \
		net.ipv6.fib_multipath_hash_policy)
	ip netns exec "$NS1" sysctl -qw net.ipv6.fib_multipath_hash_policy=1
	defer ip netns exec "$NS1" sysctl -qw \
		net.ipv6.fib_multipath_hash_policy="$saved_policy"

	block_tcp "$NS1" veth0a
	defer unblock_tcp "$NS1" veth0a
	block_tcp "$NS1" veth1a
	defer unblock_tcp "$NS1" veth1a

	ip netns exec "$NS2" socat \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr,fork" \
		EXEC:"echo POLICY1_OK" &
	defer kill_process $!

	wait_local_port_listen "$NS2" "$port" tcp

	local rehash_before
	rehash_before=$(get_netstat_counter "$NS1" TcpTimeoutRehash)

	ip netns exec "$NS1" timeout 10 socat -u \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1],connect-timeout=8" \
		STDOUT >/dev/null 2>&1 &
	local client_pid=$!
	defer kill_process "$client_pid"

	# With policy 1, the deterministic 5-tuple hash always selects
	# the same path.  Wait long enough for several SYN retransmits,
	# then verify SYNs did NOT appear on both paths.
	if slowwait 8 both_devs_attempted "$NS1" veth0a veth1a; then
		check_err 1 "SYNs appeared on both paths despite policy 1"
	fi

	# Confirm rehash was still attempted (txhash re-rolled) even
	# though the ECMP path did not change.
	local rehash_after
	rehash_after=$(get_netstat_counter "$NS1" TcpTimeoutRehash)
	if [ "$rehash_after" -le "$rehash_before" ]; then
		check_err 1 "TcpTimeoutRehash counter did not increment"
	fi

	log_test "Local ECMP policy 1: no path change on rehash"
}

# Verify that mp_hash does not leak into the on-wire flowlabel.
# With auto_flowlabels=0, the wire flowlabel must be 0.  Install tc
# filters that pass TCP with flowlabel=0 but drop TCP with nonzero
# flowlabel, then establish a connection and transfer data.  If
# mp_hash leaked into fl6->flowlabel, the SYN or data packets would
# be dropped and the connection would fail.
test_ecmp_no_flowlabel_leak()
{
	RET=0
	local port=$((PORT + 6))

	local saved_afl
	saved_afl=$(ip netns exec "$NS1" sysctl -n \
		net.ipv6.auto_flowlabels)
	ip netns exec "$NS1" sysctl -qw net.ipv6.auto_flowlabels=0
	defer ip netns exec "$NS1" sysctl -qw \
		net.ipv6.auto_flowlabels="$saved_afl"

	# On both egress interfaces: pass TCP with flowlabel=0 (prio 1),
	# drop any remaining TCP (nonzero flowlabel, prio 2).  ICMPv6
	# matches neither filter and passes through normally.
	local dev
	for dev in veth0a veth1a; do
		ip netns exec "$NS1" tc qdisc add dev "$dev" \
			root handle 1: prio
		ip netns exec "$NS1" tc filter add dev "$dev" parent 1: \
			protocol ipv6 prio 1 u32 \
			match u32 0x00000000 0x000FFFFF at 0 \
			match u8 0x06 0xff at 6 \
			action ok
		ip netns exec "$NS1" tc filter add dev "$dev" parent 1: \
			protocol ipv6 prio 2 u32 \
			match u8 0x06 0xff at 6 \
			action drop
		defer unblock_tcp "$NS1" "$dev"
	done

	ip netns exec "$NS2" socat \
		"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" \
		EXEC:"echo FLOWLABEL_OK" &
	defer kill_process $!

	wait_local_port_listen "$NS2" "$port" tcp

	local tmpfile
	tmpfile=$(mktemp)
	defer rm -f "$tmpfile"

	ip netns exec "$NS1" socat -u \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1],connect-timeout=10" \
		STDOUT >"$tmpfile" 2>&1

	local result
	result=$(cat "$tmpfile" 2>/dev/null)
	if [[ "$result" != *"FLOWLABEL_OK"* ]]; then
		check_err 1 "connection failed: mp_hash may have leaked into wire flowlabel"
	fi

	log_test "No flowlabel leak with auto_flowlabels=0"
}

# Helper: stream data, replace the ECMP route with identical nexthops
# (creating a new fib6_info and invalidating the cached dst), then
# check that traffic stays on the same path.  Used by both the normal
# tcp_v6_connect and syncookie variants.
ecmp_dst_rebuild_check()
{
	local ns_client=$1; shift
	local ns_server=$1; shift
	local port=$1; shift
	local rc=0

	# Suppress __dst_negative_advice() during the test so that a
	# real TCP timeout cannot rehash sk_txhash.  Without this, a
	# slow or loaded CI machine may fire an RTO during the sleep
	# below, changing the path legitimately and causing a false
	# failure.
	local saved_retries1
	saved_retries1=$(ip netns exec "$ns_client" sysctl -n \
		net.ipv4.tcp_retries1)
	ip netns exec "$ns_client" sysctl -qw net.ipv4.tcp_retries1=255

	local base0 base1
	base0=$(link_tx_packets_get "$ns_client" veth0a)
	base1=$(link_tx_packets_get "$ns_client" veth1a)

	ip netns exec "$ns_client" timeout 15 socat -u \
		OPEN:/dev/zero \
		"TCP6:[fd00:ff::2]:$port,bind=[fd00:ff::1]" \
		&>/dev/null &
	local client_pid=$!

	# Wait for enough packets to identify the active path.
	# Return 2 for setup failure (distinct from 1 = path changed).
	if ! busywait "$BUSYWAIT_TIMEOUT" until_counter_is \
			">= $((base0 + base1 + 50))" \
		link_tx_packets_total "$ns_client" > /dev/null; then
		ip netns exec "$ns_client" sysctl -qw \
			net.ipv4.tcp_retries1="$saved_retries1"
		kill "$client_pid" 2>/dev/null
		wait "$client_pid" 2>/dev/null
		return 2
	fi

	local mid0 mid1 active_dev inactive_dev
	mid0=$(link_tx_packets_get "$ns_client" veth0a)
	mid1=$(link_tx_packets_get "$ns_client" veth1a)
	if [ $((mid0 - base0)) -ge $((mid1 - base1)) ]; then
		active_dev=veth0a; inactive_dev=veth1a
	else
		active_dev=veth1a; inactive_dev=veth0a
	fi

	local active_before inactive_before
	active_before=$(link_tx_packets_get "$ns_client" "$active_dev")
	inactive_before=$(link_tx_packets_get "$ns_client" "$inactive_dev")

	# Replace the ECMP route with identical nexthops.
	# This creates a new fib6_info, invalidating the
	# socket's cached dst on the next ip6_dst_check().
	ip -n "$ns_client" -6 route replace fd00:ff::2/128 \
		nexthop via fd00:a::2 dev veth0a \
		nexthop via fd00:b::2 dev veth1a

	# Wait for enough post-rebuild traffic to detect a path change.
	if ! busywait "$BUSYWAIT_TIMEOUT" until_counter_is \
			">= $((active_before + inactive_before + 50))" \
		link_tx_packets_total "$ns_client" > /dev/null; then
		ip netns exec "$ns_client" sysctl -qw \
			net.ipv4.tcp_retries1="$saved_retries1"
		kill "$client_pid" 2>/dev/null
		wait "$client_pid" 2>/dev/null
		return 2
	fi

	local active_after inactive_after
	active_after=$(link_tx_packets_get "$ns_client" "$active_dev")
	inactive_after=$(link_tx_packets_get "$ns_client" "$inactive_dev")

	local active_delta=$((active_after - active_before))
	local inactive_delta=$((inactive_after - inactive_before))

	if [ "$inactive_delta" -gt "$active_delta" ]; then
		rc=1
	fi

	ip netns exec "$ns_client" sysctl -qw \
		net.ipv4.tcp_retries1="$saved_retries1"
	kill "$client_pid" 2>/dev/null
	wait "$client_pid" 2>/dev/null
	return "$rc"
}

# Run ecmp_dst_rebuild_check for ECMP_REBUILD_ROUNDS rounds, each with
# a fresh server and connection.  With a correct kernel the path is
# deterministic (same txhash always selects the same ECMP nexthop),
# so any path change is a bug.  Multiple rounds catch a buggy kernel
# that picks a random path: each round has 50% chance of accidentally
# matching, so 10 rounds gives < 0.1% false-pass probability.
ecmp_dst_rebuild_loop()
{
	local base_port=$1; shift
	local label=$1; shift
	local path_changes=0
	local r

	for r in $(seq 1 "$ECMP_REBUILD_ROUNDS"); do
		local port=$((base_port + r))

		ip netns exec "$NS2" socat -u \
			"TCP6-LISTEN:$port,bind=[fd00:ff::2],reuseaddr" \
			- >/dev/null &
		local server_pid=$!

		wait_local_port_listen "$NS2" "$port" tcp

		local check_rc=0
		ecmp_dst_rebuild_check "$NS1" "$NS2" "$port" || check_rc=$?
		if [ "$check_rc" -eq 2 ]; then
			check_err 1 "no TX activity in round $r"
			break
		elif [ "$check_rc" -eq 1 ]; then
			path_changes=$((path_changes + 1))
		fi

		kill "$server_pid" 2>/dev/null
		wait "$server_pid" 2>/dev/null
	done

	if [ "$path_changes" -gt 0 ]; then
		check_err 1 "$path_changes/$ECMP_REBUILD_ROUNDS changed path"
	fi

	log_test "$label"
}

# Verify that a natural dst invalidation (route table change) does not
# cause the connection to switch ECMP paths.  With the fix, both the
# initial route lookup (tcp_v6_connect) and subsequent rebuilds
# (inet6_csk_route_socket) use sk_txhash >> 1, so the path is stable.
test_ecmp_dst_rebuild_consistency()
{
	RET=0

	ecmp_dst_rebuild_loop "$((PORT + 7))" \
		"ECMP path stable after route replace"
}

# Same as above but with syncookies forced (tcp_syncookies=2), so the
# server creates the full socket via cookie_v6_check() instead of the
# normal three-way handshake path.
test_ecmp_dst_rebuild_syncookie_consistency()
{
	RET=0

	local saved_syncookies
	saved_syncookies=$(ip netns exec "$NS2" sysctl -n \
		net.ipv4.tcp_syncookies)
	ip netns exec "$NS2" sysctl -qw net.ipv4.tcp_syncookies=2
	defer ip netns exec "$NS2" sysctl -qw \
		net.ipv4.tcp_syncookies="$saved_syncookies"

	ecmp_dst_rebuild_loop "$((PORT + 27))" \
		"ECMP path stable after route replace (syncookies)"
}

require_command socat

trap 'defer_scopes_cleanup; cleanup_all_ns' EXIT
setup || exit $?
tests_run
exit "$EXIT_STATUS"
