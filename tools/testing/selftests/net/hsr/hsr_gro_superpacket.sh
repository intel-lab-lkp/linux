#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test HSR handling of GRO/GSO super-packets:
#
#  1. Enslaving a device to an HSR master disables GRO on it
#     (dev_disable_gro()).
#  2. The HSR master does not advertise GSO/TSO features.
#  3. A TCP stream from a TSO-enabled SAN (which therefore emits GSO
#     super-packets) is unfolded at the HSR forward entry and delivered
#     per-frame: the transfer completes with zero retransmits.
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

ns_dut="hsr-gro-dut"
ns_san="hsr-gro-san"
ns_peer="hsr-gro-peer"
san_ip="100.64.0.1"
peer_ip="100.64.0.3"
iperf_pid=""
server_wrapper=""
iperf_pidfile="/tmp/hsr_gro_iperf.pid"

cleanup()
{
	if [ -n "${iperf_pid}" ] && [[ "${iperf_pid}" =~ ^[0-9]+$ ]]; then
		kill "${iperf_pid}" 2>/dev/null
	fi
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
	# last resort, namespace-scoped only: kill iperf3 processes that
	# actually live in the peer netns. netns != pidns: a blind pkill
	# would scan the host PID space and hit unrelated tests.
	for _p in $(ip netns pids "$ns_peer" 2>/dev/null); do
		if [ "$(cat /proc/$_p/comm 2>/dev/null)" = "iperf3" ]; then
			kill "$_p" 2>/dev/null
		fi
	done
	iperf_pid=""
	rm -f "${iperf_pidfile}"
	ip netns del "$ns_dut" 2>/dev/null
	ip netns del "$ns_san" 2>/dev/null
	ip netns del "$ns_peer" 2>/dev/null
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

setup_topo()
{
	local ns

	cleanup
	for ns in "$ns_dut" "$ns_san" "$ns_peer"; do
		ip netns add "$ns"
	done

	ip link add d_a netns "$ns_dut" type veth peer name p_a netns "$ns_peer"
	ip link add d_b netns "$ns_dut" type veth peer name p_b netns "$ns_peer"
	ip link add d_il netns "$ns_dut" type veth peer name s0 netns "$ns_san"

	# HSR tags add 6 bytes per frame; give the LAN legs headroom.
	for iface in d_a d_b; do
		nsx "$ns_dut" "ip link set $iface mtu 1600; ip link set $iface up"
	done
	for iface in p_a p_b; do
		nsx "$ns_peer" "ip link set $iface mtu 1600; ip link set $iface up"
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
	stop_if_error "HSR master still advertises GSO/TSO."
}

start_iperf_server()
{
	# One-shot server, no -D: record its exact PID (netns shares the PID
	# namespace). The wrapping shell waits on the server so the script can
	# really wait(1) for the whole tree, on success and failure alike.
	rm -f "${iperf_pidfile}"
	( nsx "$ns_peer" "iperf3 -s -1 > /dev/null 2>&1 & echo \$! > ${iperf_pidfile}; wait" ) &
	server_wrapper=$!
	# the wrapper writes the pidfile asynchronously; wait for it to
	# appear instead of racing the read
	for _ in $(seq 1 50); do
		[ -s "${iperf_pidfile}" ] && break
		sleep 0.1
	done
	if [ ! -s "${iperf_pidfile}" ]; then
		echo "FAIL: iperf3 server did not publish a pid (no pidfile)" 1>&2
		ret=1
		return 1
	fi
	iperf_pid=$(cat "${iperf_pidfile}")
	if ! [[ "${iperf_pid}" =~ ^[0-9]+$ ]] || ! kill -0 "${iperf_pid}" 2>/dev/null; then
		echo "FAIL: iperf3 server pid '${iperf_pid}' invalid or not alive" 1>&2
		ret=1
		return 1
	fi
	sleep 1
	return 0
}

do_tso_stream_test()
{
	local out sender_retr

	echo "INFO: Enabling TSO/GSO on the SAN interface."
	nsx "$ns_san" "ethtool -K s0 tso on gso on"
	check_feature "$ns_san" s0 tcp-segmentation-offload on
	stop_if_error "Could not enable TSO on the SAN interface."

	echo "INFO: Running 10s TCP stream SAN -> peer through the HSR DUT."
	start_iperf_server || return
		# rate-capped: the discriminator is per-frame unfold (completion with
	# 0 retransmits), not max throughput; uncapped runs flap at VM/CI
	# edge rates without indicating a functional problem.
	out=$(nsx "$ns_san" "timeout 60 iperf3 -c $peer_ip -M 1446 -b 2G -t 10" 2>&1)
	if [ $? -ne 0 ]; then
		echo "FAIL: iperf3 client failed:" 1>&2
		echo "$out" 1>&2
		ret=1
		return
	fi

	# success path: the one-shot server exits by itself; really wait for
	# the wrapper to reap it, and check its exit status
	wait "${server_wrapper}"
	server_rc=$?
	server_wrapper=""
	iperf_pid=""
	if [ "$server_rc" -ne 0 ]; then
		echo "FAIL: iperf3 server exited with rc=$server_rc" 1>&2
		ret=1
		return
	fi

	sender_retr=$(echo "$out" | awk '/sender/ {print $(NF-1)}')
	if [ -z "$sender_retr" ] || [ "$sender_retr" -ne 0 ]; then
		echo "FAIL: retransmits during GSO unfold: ${sender_retr:-unknown}" 1>&2
		echo "$out" 1>&2
		ret=1
		return
	fi

	echo "INFO: TCP stream completed with 0 retransmits [ OK ]"
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
exit $ret
