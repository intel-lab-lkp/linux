#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test HSR handling of GRO/GSO super-packets:
#
#  1. Enslaving a device to an HSR master disables GRO on it
#     (dev_disable_gro()).
#  2. The HSR master does not advertise GSO/TSO features.
#  3. A TCP stream from a TSO-enabled SAN (which therefore emits GSO
#     super-packets) is unfolded at the HSR forward entry. Evidence:
#     interface-counter deltas show super-packet-sized frames leaving
#     the SAN and per-frame-sized traffic leaving the DUT's LAN ports.
#
# Topology (100.64.0.0/24):
#
#   ns_san                    ns_dut                      ns_peer
#  +-----------+  interlink  +---------------+  LAN A/B  +-----------+
#  | s0 [0.1]  |-------------| d_il   hsr0   |-----------| hsr1 [0.3]|
#  +-----------+             | d_a / d_b     |           | p_a / p_b |
#                            +---------------+           +-----------+
#
# SAN traffic reaches ns_peer only through hsr0's forward path
# (interlink RX -> LAN A/B TX), so every SAN frame is tagged and
# forwarded by the DUT.

source ./hsr_common.sh

san_ip="100.64.0.1"
peer_ip="100.64.0.3"

# Aggregate counter thresholds for the stream test (bytes/packets):
# SAN_AVG_MIN proves GSO super-packets left the SAN; LAN_AVG_MAX is a
# guard with margin, not the protocol maximum (see do_tso_stream_test).
SAN_AVG_MIN=2048
LAN_AVG_MAX=1514

iperf_pid=""
server_wrapper=""
workdir=""
pidfile=""
ns_dut=""
ns_san=""
ns_peer=""
rcfile=""

cleanup()
{
	# exact-PID kill only after RE-validating identity (guards against
	# PID reuse between publication and cleanup)
	if [ -n "${iperf_pid}" ] && valid_server_pid "${iperf_pid}"; then
		kill "${iperf_pid}" 2>/dev/null
	fi
	iperf_pid=""
	if [ -n "${server_wrapper}" ]; then
		# the wrapper waits on the server; reap it with a 5s bound so a
		# live-but-unpublished server can never hang cleanup
		for _ in $(seq 1 50); do
			kill -0 "${server_wrapper}" 2>/dev/null || break
			sleep 0.1
		done
		kill "${server_wrapper}" 2>/dev/null
		wait "${server_wrapper}" 2>/dev/null
		server_wrapper=""
	fi
	# last resort, namespace-scoped only: TERM the iperf3 processes that
	# actually live in the peer netns, poll for bounded exit, then
	# SIGKILL any survivor before touching the namespace name. A blind
	# pkill would scan the host PID space and hit unrelated tests.
	local _p _still
	for _p in $(ip netns pids "$ns_peer" 2>/dev/null); do
		if is_iperf3_pid "$_p"; then
			kill "$_p" 2>/dev/null
		fi
	done
	for _ in $(seq 1 50); do
		_still=0
		for _p in $(ip netns pids "$ns_peer" 2>/dev/null); do
			if is_iperf3_pid "$_p"; then
				_still=1
				break
			fi
		done
		[ "$_still" -eq 0 ] && break
		sleep 0.1
	done
	for _p in $(ip netns pids "$ns_peer" 2>/dev/null); do
		if is_iperf3_pid "$_p"; then
			kill -9 "$_p" 2>/dev/null
		fi
	done
	# remove only the known non-empty private directory
	if [ -n "${workdir}" ] && [ -d "${workdir}" ]; then
		rm -rf "${workdir}"
	fi
	workdir=""
	pidfile=""
	rcfile=""
	cleanup_all_ns
}

trap cleanup EXIT

check_tool()
{
	if ! command -v "$1" > /dev/null 2>&1; then
		echo "SKIP: Could not run test without $1"
		exit $ksft_skip
	fi
}

nsx()
{
	ip netns exec "$1" bash -c "$2"
}

is_iperf3_pid()
{
	[ "$(cat /proc/"$1"/comm 2>/dev/null)" = "iperf3" ]
}

# Decimal-counter validation for the snapshot blocks: every value must
# be a plain decimal number. A parse failure in read_tx_counters yields
# empty/garbled fields, which this check turns into an immediate FAIL.
valid_decimals()
{
	local v

	for v in "$@"; do
		[[ "$v" =~ ^[0-9]+$ ]] || return 1
	done
	return 0
}

setup_topo()
{
	setup_ns ns_dut ns_san ns_peer || exit $?

	ip link add d_a netns "$ns_dut" type veth peer name p_a netns "$ns_peer"
	ip link add d_b netns "$ns_dut" type veth peer name p_b netns "$ns_peer"
	ip link add d_il netns "$ns_dut" type veth peer name s0 netns "$ns_san"

	# HSR tags add 6 bytes per frame; give the LAN legs headroom.
	for iface in d_a d_b; do
		nsx "$ns_dut" "ip link set $iface mtu 1600; \
			ip link set $iface up"
	done
	for iface in p_a p_b; do
		nsx "$ns_peer" "ip link set $iface mtu 1600; \
			ip link set $iface up"
	done

	nsx "$ns_dut" "ip link set d_il up"
	nsx "$ns_san" "ip link set s0 up; ip addr add $san_ip/24 dev s0"

	nsx "$ns_dut" "ip link add hsr0 type hsr \
		slave1 d_a slave2 d_b interlink d_il proto 0; \
		ip link set hsr0 up"
	nsx "$ns_peer" "ip link add hsr1 type hsr \
		slave1 p_a slave2 p_b proto 0; \
		ip link set hsr1 up; ip addr add $peer_ip/24 dev hsr1"

	# Let the nodes see each other's supervision frames.
	sleep 2
}

check_feature()
{
	local ns="$1"
	local iface="$2"
	local feature="$3"
	local want="$4"

	if nsx "$ns" "ethtool -k $iface" | grep -q "^$feature: $want"; then
		echo "INFO: $ns/$iface $feature is $want [ OK ]"
	else
		echo "FAIL: $ns/$iface $feature is not $want" 1>&2
		ret=1
	fi
}

# Off-or-absent variant: fails only when the feature is present AND on,
# so devices that simply do not list the feature do not fail it.
check_feature_not_on()
{
	local ns="$1"
	local iface="$2"
	local feature="$3"

	if nsx "$ns" "ethtool -k $iface" | grep -q "^$feature: on"; then
		echo "FAIL: $ns/$iface $feature is on" 1>&2
		ret=1
	else
		echo "INFO: $ns/$iface $feature not on [ OK ]"
	fi
}

do_gro_feature_checks()
{
	echo "INFO: Checking that enslavement disabled GRO."
	check_feature "$ns_dut" d_a generic-receive-offload off
	check_feature "$ns_dut" d_b generic-receive-offload off
	check_feature "$ns_dut" d_il generic-receive-offload off
	stop_if_error "GRO not disabled on enslaved devices."

	echo "INFO: Checking that enslavement disabled HW-GRO."
	check_feature "$ns_dut" d_a rx-gro-hw off
	check_feature "$ns_dut" d_b rx-gro-hw off
	check_feature "$ns_dut" d_il rx-gro-hw off
	stop_if_error "HW-GRO not disabled on enslaved devices."

	echo "INFO: Checking that the HSR master does not advertise GSO/TSO."
	check_feature "$ns_dut" hsr0 generic-segmentation-offload off
	check_feature "$ns_dut" hsr0 tcp-segmentation-offload off
	check_feature_not_on "$ns_dut" hsr0 tx-tcp6-segmentation
	check_feature_not_on "$ns_dut" hsr0 tx-udp-segmentation
	check_feature_not_on "$ns_dut" hsr0 tx-gso-list
	stop_if_error "HSR master still advertises GSO-family features."
}

alloc_workdir()
{
	# Allocated only here, long after the initial topology cleanup, so
	# cleanup() at setup_topo() time can never remove it. mktemp failure
	# is a hard test failure.
	workdir=$(mktemp -d /tmp/hsr_gro_test.XXXXXX) || {
		echo "FAIL: mktemp -d failed" 1>&2
		exit 1
	}
	chmod 700 "${workdir}"
	pidfile="${workdir}/iperf.pid"
	rcfile="${workdir}/iperf.rc"
}

# Numeric, alive, comm == iperf3, and really owned by the peer netns.
valid_server_pid()
{
	local p="$1"

	[[ "$p" =~ ^[0-9]+$ ]] || return 1
	kill -0 "$p" 2>/dev/null || return 1
	[ "$(cat /proc/"$p"/comm 2>/dev/null)" = "iperf3" ] || return 1
	ip netns pids "$ns_peer" 2>/dev/null | grep -qx "$p"
}

start_iperf_server()
{
	local candidate_pid

	# One-shot server, no -D: the wrapper records its exact PID and its
	# real exit status (netns shares the PID namespace and the host fs).
	alloc_workdir
	( nsx "$ns_peer" "iperf3 -s -1 > /dev/null 2>&1 & \
		echo \$! > ${pidfile}; \
		wait \$!; \
		echo \$? > ${rcfile}" ) &
	server_wrapper=$!
	# the wrapper writes the pidfile asynchronously; wait for it to
	# appear instead of racing the read
	for _ in $(seq 1 50); do
		[ -s "${pidfile}" ] && break
		sleep 0.1
	done
	if [ ! -s "${pidfile}" ]; then
		echo "FAIL: iperf3 server did not publish a pid" \
			"(no pidfile)" 1>&2
		ret=1
		return 1
	fi
	candidate_pid=$(<"${pidfile}")
	if ! valid_server_pid "${candidate_pid}"; then
		echo "FAIL: iperf3 server pid '${candidate_pid}'" \
			"failed validation" 1>&2
		ret=1
		return 1
	fi
	# publish only after full validation
	iperf_pid="${candidate_pid}"
	sleep 1
	return 0
}

# Print "<bytes> <packets>" for exactly one TX record of ns/dev; anything
# else (missing, duplicated, non-numeric) is a hard FAIL.
read_tx_counters()
{
	local ns="$1" dev="$2"
	local out cnt

	out=$(nsx "$ns" "ip -s link show $dev" | \
		awk '/^ +TX:/{getline; print $1, $2}')
	cnt=$(echo "$out" | grep -c '^[0-9]* [0-9]*$')
	if [ "$cnt" -ne 1 ]; then
		echo "FAIL: cannot parse TX counters of $ns/$dev" \
			"(records=$cnt)" 1>&2
		return 1
	fi
	echo "$out"
	return 0
}

eval_counter_delta()
{
	local name="$1" b0="$2" p0="$3" b1="$4" p1="$5" op="$6" limit="$7"
	local bd pd

	if ! [[ "$b0" =~ ^[0-9]+$ && "$b1" =~ ^[0-9]+$ && \
		"$p0" =~ ^[0-9]+$ && "$p1" =~ ^[0-9]+$ ]]; then
		echo "FAIL: non-numeric counter input for $name" 1>&2
		ret=1
		return 1
	fi
	bd=$((b1 - b0))
	pd=$((p1 - p0))
	if [ "$bd" -lt 0 ] || [ "$pd" -le 0 ]; then
		echo "FAIL: counter delta invalid for $name" \
			"(bytes=$bd pkts=$pd)" 1>&2
		ret=1
		return 1
	fi
	if [ "$op" = "gt" ]; then
		if [ "$bd" -le $((pd * limit)) ]; then
			echo "FAIL: $name bytes/packets $bd/$pd <= $limit" 1>&2
			ret=1
			return 1
		fi
	else
		if [ "$bd" -gt $((pd * limit)) ]; then
			echo "FAIL: $name bytes/packets $bd/$pd > $limit" 1>&2
			ret=1
			return 1
		fi
	fi
	echo "INFO: $name counter delta bytes=$bd packets=$pd" \
		"(op $op limit $limit) [ OK ]"
	return 0
}

do_tso_stream_test()
{
	local out sender_retr server_rc
	local san_b0 san_p0 san_b1 san_p1
	local a_b0 a_p0 a_b1 a_p1 b_b0 b_p0 b_b1 b_p1

	echo "INFO: Enabling TSO/GSO on the SAN interface."
	nsx "$ns_san" "ethtool -K s0 tso on gso on"
	check_feature "$ns_san" s0 tcp-segmentation-offload on
	stop_if_error "Could not enable TSO on the SAN interface."

	echo "INFO: Running 10s TCP stream SAN -> peer through the HSR DUT."
	start_iperf_server || return

	# Counter snapshots around the stream window. The SAN-side average
	# must exceed SAN_AVG_MIN (aggregate proof that GSO super-packets
	# really left the SAN); each DUT LAN leg must stay under LAN_AVG_MAX
	# (aggregate proof that bulk output was segmented per-frame). These
	# are aggregate discriminators, not a per-frame maximum proof.
	san_b0=0; san_p0=0; a_b0=0; a_p0=0; b_b0=0; b_p0=0
	read -r san_b0 san_p0 <<EOF
$(read_tx_counters "$ns_san" s0)
EOF
	read -r a_b0 a_p0 <<EOF
$(read_tx_counters "$ns_dut" d_a)
EOF
	read -r b_b0 b_p0 <<EOF
$(read_tx_counters "$ns_dut" d_b)
EOF
	if ! valid_decimals "$san_b0" "$san_p0" "$a_b0" "$a_p0" \
		"$b_b0" "$b_p0"; then
		echo "FAIL: baseline TX counter snapshot invalid" 1>&2
		ret=1
		return 1
	fi

	# rate-capped: the PRIMARY discriminator is the counter inequality
	# above, not max throughput; retransmits are informational only.
	# Uncapped runs flap at VM/CI edge rates without indicating a
	# functional problem.
	if ! out=$(nsx "$ns_san" "timeout 60 iperf3 -c $peer_ip -M 1446 \
		-b 2G -t 10" 2>&1); then
		echo "FAIL: iperf3 client failed:" 1>&2
		echo "$out" 1>&2
		ret=1
		return
	fi

	read -r san_b1 san_p1 <<EOF
$(read_tx_counters "$ns_san" s0)
EOF
	read -r a_b1 a_p1 <<EOF
$(read_tx_counters "$ns_dut" d_a)
EOF
	read -r b_b1 b_p1 <<EOF
$(read_tx_counters "$ns_dut" d_b)
EOF
	if ! valid_decimals "$san_b1" "$san_p1" "$a_b1" "$a_p1" \
		"$b_b1" "$b_p1"; then
		echo "FAIL: final TX counter snapshot invalid" 1>&2
		ret=1
		return 1
	fi

	eval_counter_delta "SAN s0 TX" "$san_b0" "$san_p0" "$san_b1" "$san_p1" \
		gt "$SAN_AVG_MIN"
	eval_counter_delta "DUT d_a TX" "$a_b0" "$a_p0" "$a_b1" "$a_p1" \
		le "$LAN_AVG_MAX"
	eval_counter_delta "DUT d_b TX" "$b_b0" "$b_p0" "$b_b1" "$b_p1" \
		le "$LAN_AVG_MAX"
	[ "${ret:-0}" -eq 0 ] || return

	# success path: the one-shot server exits by itself; reap the
	# wrapper, then REQUIRE the rcfile with the server's real status
	wait "${server_wrapper}"
	server_wrapper=""
	if [ ! -s "${rcfile}" ]; then
		echo "FAIL: iperf3 server status file missing (${rcfile})" 1>&2
		ret=1
		return
	fi
	server_rc=$(cat "${rcfile}")
	if ! [[ "$server_rc" =~ ^[0-9]+$ ]] || [ "$server_rc" -ne 0 ]; then
		echo "FAIL: iperf3 server exited with rc='${server_rc}'" 1>&2
		ret=1
		return
	fi
	iperf_pid=""

	# secondary health signal only: anchored, single-match, numeric —
	# any parse anomaly is a loud FAIL, but the value itself no longer
	# gates (the counter inequalities above are the primary evidence).
	sender_retr=$(echo "$out" | awk '/sec .* sender$/ {print $(NF-1)}')
	if [ "$(echo "$sender_retr" | grep -Ec '^[0-9]+$')" -ne 1 ]; then
		echo "FAIL: cannot parse sender retransmits reliably" 1>&2
		echo "$out" 1>&2
		ret=1
		return
	fi
	echo "INFO: TCP stream done;" \
		"sender retransmits=$sender_retr (secondary signal)"
	echo "$out" | grep -E "sender|receiver"
}

check_prerequisites
check_tool ethtool
check_tool iperf3
check_tool timeout

# iproute2 must know the HSR interlink syntax.
if ! ip link help hsr 2>&1 | grep -qi interlink; then
	echo "SKIP: iproute2 has no HSR interlink support"
	exit $ksft_skip
fi

setup_topo

echo "INFO: Initial validation ping (SAN -> peer through the DUT)."
do_ping "$ns_san" "$peer_ip"
stop_if_error "Initial validation failed."

do_gro_feature_checks
do_tso_stream_test
stop_if_error "GSO super-packet stream test failed."

echo "INFO: All good."
cleanup
exit $ret
