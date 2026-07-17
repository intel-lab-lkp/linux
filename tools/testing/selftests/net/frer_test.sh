#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright 2026 NXP
#
# frer_test.sh - IEEE 802.1CB FRER tc action kselftest
#
# Topology for tests 1-4:
#
#   ns_talker  bond0 (veth_a0 + veth_b0)  <--->  bond1 (veth_a1 + veth_b1)  ns_listener
#
#   IP_SRC assigned to bond0;  IP_DST assigned to bond1
#
#   bond mode: balance-rr (round-robin), so frames are distributed across
#              both slaves.  FRER push is configured on both veth_a0 and
#              veth_b0 egress with cross-mirror so every frame sent by either
#              slave carries an R-TAG and a mirrored copy reaches the peer.
#   FRER recover: veth_a1/veth_b1 ingress, shared or individual recover per test
#
#   Ping runs from bond0 to bond1; tcpdump captures on bond1 (or on individual
#   slave interfaces for tests where both copies must be observable).
#
# Test 5: simple point-to-point, self-contained topology (no bond).
# Test 6: relay system, self-contained topology.
#
# All namespaces, veth pairs, bond interfaces, tc rules and addresses are
# created and destroyed within this script.  External dependencies:
#   - kernel with CONFIG_NET_ACT_FRER and CONFIG_BONDING
#   - iproute2 tc with frer action support
#   - tcpdump, ping
#   - root privileges

# ----------------------------------------------------------------------------
# kselftest library: TAP output + exit-code constants
# ----------------------------------------------------------------------------
ksft_lib="${KSFT_LIB:-$(dirname "$0")/../kselftest/lib.sh}"
if [ -f "$ksft_lib" ]; then
	# shellcheck source=/dev/null
	. "$ksft_lib"
else
	# Minimal fallback when run outside the kselftest tree
	KSFT_PASS=0
	KSFT_FAIL=1
	KSFT_SKIP=4
	_ksft_count=0
	_ksft_pass=0
	_ksft_fail=0
	_ksft_skip=0

	ksft_print_header() { echo "TAP version 13"; }
	ksft_set_plan()     { echo "1..$1"; }
	ksft_test_result_pass() {
		_ksft_count=$((_ksft_count + 1)); _ksft_pass=$((_ksft_pass + 1))
		echo "ok $_ksft_count - $*"
	}
	ksft_test_result_fail() {
		_ksft_count=$((_ksft_count + 1)); _ksft_fail=$((_ksft_fail + 1))
		echo "not ok $_ksft_count - $*"
	}
	ksft_test_result_skip() {
		_ksft_count=$((_ksft_count + 1)); _ksft_skip=$((_ksft_skip + 1))
		echo "ok $_ksft_count - $* # SKIP"
	}
	ksft_print_cnts() {
		echo "# Totals: pass=$_ksft_pass fail=$_ksft_fail skip=$_ksft_skip"
	}
	ksft_exit_pass()     { exit $KSFT_PASS; }
	ksft_exit_fail()     { exit $KSFT_FAIL; }
	ksft_exit_fail_msg() { echo "# FATAL: $*" >&2; exit $KSFT_FAIL; }
fi

# ----------------------------------------------------------------------------
# Configuration (override via environment)
# ----------------------------------------------------------------------------
TC="${TC:-tc}"
PING="${PING:-ping}"
TCPDUMP="${TCPDUMP:-tcpdump}"
PING_COUNT="${PING_COUNT:-5}"
PING_TIMEOUT="${PING_TIMEOUT:-2}"
SKIP_MODPROBE="${SKIP_MODPROBE:-0}"

# Bond topology interfaces (tests 1-4)
readonly VETH_A0="frer_a0"
readonly VETH_A1="frer_a1"
readonly VETH_B0="frer_b0"
readonly VETH_B1="frer_b1"
readonly BOND0="frer_bond0"
readonly BOND1="frer_bond1"

readonly NS_TALKER="frer_ns_talker"
readonly NS_LISTENER="frer_ns_listener"

readonly IP_SRC="10.0.0.1"
readonly IP_DST="10.0.0.2"

# Point-to-point topology interfaces (test 5)
readonly P2P_NS_SRC="frer_p2p_src"
readonly P2P_NS_DST="frer_p2p_dst"
readonly P2P_VETH_A0="frer_p2p_a0"
readonly P2P_VETH_A1="frer_p2p_a1"
readonly IP_P2P_SRC="10.0.1.1"
readonly IP_P2P_DST="10.0.1.2"

# Relay topology interfaces (test 6)
#
#   ns_talker (talker_eth.100) -- talker_eth/br0_uplink -- bridge0 (br_r0)
#                                         |-- br0_swp0/br1_swp0 --\
#                                         \-- br0_swp1/br1_swp1 --+--\
#		bridge1 (br_r1) -- br1_downlink/listener_eth -- ns_listener
#
# bridge0 acts as sequence generator (frer push + replicate to both paths).
# bridge1 acts as eliminator (frer recover, shared, tag-pop).
readonly R_NS_TALKER="frer_r_talker"
readonly R_NS_BRIDGE0="frer_r_bridge0"
readonly R_NS_BRIDGE1="frer_r_bridge1"
readonly R_NS_LISTENER="frer_r_listener"
readonly R_TALKER_ETH="r_tlk_eth"       # talker-side physical port
readonly R_BR0_UPLINK="r_br0_uplink"    # bridge0 uplink facing talker
readonly R_BR0_SWP0="r_br0_swp0"        # bridge0 redundant path port 0
readonly R_BR0_SWP1="r_br0_swp1"        # bridge0 redundant path port 1
readonly R_BR1_SWP0="r_br1_swp0"        # bridge1 redundant path port 0
readonly R_BR1_SWP1="r_br1_swp1"        # bridge1 redundant path port 1
readonly R_BR1_DOWNLINK="r_br1_dwnlnk"  # bridge1 downlink facing listener
readonly R_LISTENER_ETH="r_lst_eth"     # listener-side physical port
readonly R_BR0="br_r0"
readonly R_BR1="br_r1"
readonly R_VLAN=100
readonly R_IP_TALKER="10.1.0.1"
readonly R_IP_LISTENER="10.1.0.2"

# FRER action index constants
readonly IDX_PUSH=1
readonly IDX_SHARED_RCVY=10
readonly IDX_INDV_RCVY_A=20
readonly IDX_INDV_RCVY_B=21
readonly IDX_NO_POP=30
readonly IDX_P2P_RCVY=40
readonly IDX_RELAY_PUSH=50
readonly IDX_RELAY_RCVY=60

readonly NUM_TESTS=6

# ----------------------------------------------------------------------------
# Prerequisite check
# ----------------------------------------------------------------------------
check_prerequisites()
{
	local missing=0

	[ "$(id -u)" -eq 0 ] || { echo "# Must be run as root" >&2; missing=1; }

	for cmd in ip "$TC" "$TCPDUMP" "$PING"; do
		command -v "$cmd" >/dev/null 2>&1 || {
			echo "# Missing command: $cmd" >&2
			missing=1
		}
	done

	if [ "$missing" -ne 0 ]; then
		ksft_set_plan "$NUM_TESTS"
		for i in $(seq 1 "$NUM_TESTS"); do
			ksft_test_result_skip "prerequisites not met (test $i)"
		done
		ksft_print_cnts
		exit "$KSFT_SKIP"
	fi
}

load_module()
{
	[ "$SKIP_MODPROBE" = "1" ] && return
	if ! modprobe act_frer 2>/dev/null; then
		echo "# modprobe act_frer failed - may be built-in or unavailable" >&2
	fi
	if ! modprobe bonding 2>/dev/null; then
		echo "# modprobe bonding failed - may be built-in or unavailable" >&2
	fi
}

check_frer_action()
{
	ip netns exec "$NS_TALKER" \
		$TC actions add action frer push index 999 2>/dev/null || return 1
	ip netns exec "$NS_TALKER" \
		$TC actions del action frer index 999 2>/dev/null || true
	return 0
}

# ----------------------------------------------------------------------------
# Bond topology setup / teardown (used by tests 1-4)
# ----------------------------------------------------------------------------
setup_topology()
{
	for n in "$NS_TALKER" "$NS_LISTENER"; do
		ip netns add "$n"
	done

	ip link add "$VETH_A0" type veth peer name "$VETH_A1"
	ip link set "$VETH_A0" netns "$NS_TALKER"
	ip link set "$VETH_A1" netns "$NS_LISTENER"

	ip link add "$VETH_B0" type veth peer name "$VETH_B1"
	ip link set "$VETH_B0" netns "$NS_TALKER"
	ip link set "$VETH_B1" netns "$NS_LISTENER"

	# ns_talker: create bond0 (balance-rr), frames round-robin across both slaves.
	ip netns exec "$NS_TALKER" ip link set lo up
	ip netns exec "$NS_TALKER" ip link add "$BOND0" type bond mode balance-rr miimon 100
	ip netns exec "$NS_TALKER" ip link set "$VETH_A0" master "$BOND0"
	ip netns exec "$NS_TALKER" ip link set "$VETH_B0" master "$BOND0"
	ip netns exec "$NS_TALKER" ip link set "$VETH_A0" up
	ip netns exec "$NS_TALKER" ip link set "$VETH_B0" up
	ip netns exec "$NS_TALKER" ip link set "$BOND0" up
	ip netns exec "$NS_TALKER" ip addr add "${IP_SRC}/24" dev "$BOND0"

	# ns_listener: create bond1 (balance-rr).
	ip netns exec "$NS_LISTENER" ip link set lo up
	ip netns exec "$NS_LISTENER" ip link add "$BOND1" type bond mode balance-rr miimon 100
	ip netns exec "$NS_LISTENER" ip link set "$VETH_A1" master "$BOND1"
	ip netns exec "$NS_LISTENER" ip link set "$VETH_B1" master "$BOND1"
	ip netns exec "$NS_LISTENER" ip link set "$VETH_A1" up
	ip netns exec "$NS_LISTENER" ip link set "$VETH_B1" up
	ip netns exec "$NS_LISTENER" ip link set "$BOND1" up
	ip netns exec "$NS_LISTENER" ip addr add "${IP_DST}/24" dev "$BOND1"

	# Static ARP so L2 forwarding works without ARP broadcasts.
	# With balance-rr both slaves share the bond MAC.
	local mac_bond0 mac_bond1
	mac_bond0=$(ip netns exec "$NS_TALKER"   cat /sys/class/net/"$BOND0"/address)
	mac_bond1=$(ip netns exec "$NS_LISTENER" cat /sys/class/net/"$BOND1"/address)
	ip netns exec "$NS_TALKER"   ip neigh add "$IP_DST" lladdr "$mac_bond1" dev "$BOND0"
	ip netns exec "$NS_LISTENER" ip neigh add "$IP_SRC" lladdr "$mac_bond0" dev "$BOND1"
}

cleanup()
{
	for n in "$NS_TALKER" "$NS_LISTENER" \
		"$P2P_NS_SRC" "$P2P_NS_DST" \
		"$R_NS_TALKER" "$R_NS_BRIDGE0" "$R_NS_BRIDGE1" "$R_NS_LISTENER"; do
		ip netns del "$n" 2>/dev/null || true
	done
}
trap cleanup EXIT

# ----------------------------------------------------------------------------
# TC rule helpers
# ----------------------------------------------------------------------------

# Push on both veth_a0 and veth_b0 egress using the same shared frer push
# action (IDX_PUSH).  Each slave also mirrors to the other so that every
# outgoing frame is replicated onto both paths regardless of which slave the
# bond currently selects.  This prevents packet loss during bond link changes.
setup_push_mirror()
{
	ip netns exec "$NS_TALKER" $TC qdisc add dev "$VETH_A0" clsact
	ip netns exec "$NS_TALKER" $TC filter add dev "$VETH_A0" egress \
		protocol ip flower skip_hw \
		action frer push index $IDX_PUSH \
		action mirred egress mirror dev "$VETH_B0"

	ip netns exec "$NS_TALKER" $TC qdisc add dev "$VETH_B0" clsact
	ip netns exec "$NS_TALKER" $TC filter add dev "$VETH_B0" egress \
		protocol ip flower skip_hw \
		action frer push index $IDX_PUSH \
		action mirred egress mirror dev "$VETH_A0"
}

teardown_tc()
{
	for dev in "$VETH_A0" "$VETH_B0"; do
		ip netns exec "$NS_TALKER" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	for dev in "$VETH_A1" "$VETH_B1"; do
		ip netns exec "$NS_LISTENER" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	ip netns exec "$NS_TALKER"   $TC actions flush action frer 2>/dev/null || true
	ip netns exec "$NS_LISTENER" $TC actions flush action frer 2>/dev/null || true
}

# ----------------------------------------------------------------------------
# Packet-capture helpers
#
# capture_start_on NS IFACE PCAP [BPF_FILTER]
#   Starts tcpdump in namespace NS on IFACE, writing to PCAP.
#   Stores PID in _CAP_PID.
#
# capture_stop
#   Waits for tcpdump (stored in _CAP_PID) to finish.
#
# capture_count_on NS PCAP
#   Prints the number of captured packets.
#
# Convenience wrappers capture_start / capture_count target bond1 in
# NS_LISTENER (the primary observation point for tests 2 and 4).
# ----------------------------------------------------------------------------
_CAP_PID=""

capture_start_on()
{
	local ns="$1" iface="$2" pcap="$3" filter="${4:-}"

	if [ -n "$filter" ]; then
		ip netns exec "$ns" timeout 4 \
			$TCPDUMP -i "$iface" -w "$pcap" \
			--immediate-mode -Z root -y EN10MB \
			$filter >/dev/null 2>&1 &
	else
		ip netns exec "$ns" timeout 4 \
			$TCPDUMP -i "$iface" -w "$pcap" \
			--immediate-mode -Z root -y EN10MB \
			>/dev/null 2>&1 &
	fi
	_CAP_PID=$!

	# Wait until tcpdump opens a packet socket (max ~2.5 s).
	local tries=0
	while [ $tries -lt 50 ]; do
		ip netns exec "$ns" grep -q "$iface" /proc/net/packet 2>/dev/null && break
		sleep 0.05
		tries=$((tries + 1))
	done
}

capture_stop()
{
	[ -n "$_CAP_PID" ] || return 0
	wait "$_CAP_PID" 2>/dev/null || true
	_CAP_PID=""
}

capture_count_on()
{
	local ns="$1" pcap="$2"
	ip netns exec "$ns" \
		$TCPDUMP -r "$pcap" --no-promiscuous-mode 2>/dev/null \
		| grep -c "^[0-9]" || true
}

# Convenience wrappers: default to bond1 in NS_LISTENER
capture_start() { capture_start_on "$NS_LISTENER" "$BOND1" "$@"; }
capture_count() { capture_count_on "$NS_LISTENER" "$1"; }

# ----------------------------------------------------------------------------
# Ping helper
# ----------------------------------------------------------------------------
do_ping()
{
	local rc=0
	ip netns exec "$NS_TALKER" \
		$PING -c "$PING_COUNT" -W "$PING_TIMEOUT" -i 0.2 -q \
		"$IP_DST" >/dev/null 2>&1 || rc=$?
	return $rc
}

# ----------------------------------------------------------------------------
# tc statistics parser
# ----------------------------------------------------------------------------
tc_stat()
{
	local dump="$1" field="$2"
	echo "$dump" | awk -F"${field}=" 'NF>1{split($2,a," ");print a[1];exit}' || echo "0"
}

# ----------------------------------------------------------------------------
# TEST 1: PUSH VERIFY (bond topology)
#
# Only push is configured on the talker side; no recover on the listener.
# The push action on veth_a0 egress inserts an R-TAG and mirrors a copy to
# veth_b0, so both listener slaves (veth_a1 and veth_b1) receive a frame
# with EtherType 0xF1C1.  Captures run sequentially on each slave to verify
# that both paths carry R-TAG frames.
#
# Pass criteria:
#   - veth_a1 captures >= 1 R-TAG frame
#   - veth_b1 captures >= 1 R-TAG frame
# ----------------------------------------------------------------------------
test_push_verify_bond()
{
	local pcap_a pcap_b cap_a cap_b
	local result="pass"

	setup_push_mirror

	# Capture 1: R-TAG frames on veth_a1 (path A)
	pcap_a=$(mktemp /tmp/frer_bond_push_a_XXXXXX.pcap)
	capture_start_on "$NS_LISTENER" "$VETH_A1" "$pcap_a" "ether proto 0xf1c1"
	ip netns exec "$NS_TALKER" \
		$PING -c 3 -W 1 -i 0.2 -q "$IP_DST" >/dev/null 2>&1 || true
	capture_stop
	cap_a=$(capture_count_on "$NS_LISTENER" "$pcap_a")
	rm -f "$pcap_a"

	# Capture 2: R-TAG frames on veth_b1 (path B, mirrored copy)
	pcap_b=$(mktemp /tmp/frer_bond_push_b_XXXXXX.pcap)
	capture_start_on "$NS_LISTENER" "$VETH_B1" "$pcap_b" "ether proto 0xf1c1"
	ip netns exec "$NS_TALKER" \
		$PING -c 3 -W 1 -i 0.2 -q "$IP_DST" >/dev/null 2>&1 || true
	capture_stop
	cap_b=$(capture_count_on "$NS_LISTENER" "$pcap_b")
	rm -f "$pcap_b"

	teardown_tc

	echo "# bond push verify: veth_a1 R-TAG=$cap_a veth_b1 R-TAG=$cap_b"

	[ "$cap_a" -ge 1 ] || result="fail"
	[ "$cap_b" -ge 1 ] || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"bond push verify: R-TAG on both paths (a1=$cap_a b1=$cap_b)"
	else
		ksft_test_result_fail \
			"bond push verify: expected R-TAG on both paths (a1=$cap_a b1=$cap_b)"
	fi
}

# ----------------------------------------------------------------------------
# TEST 2: SHARED RECOVER E2E (bond topology)
#
# veth_a1 and veth_b1 ingress share one recover action (idx=10) with tag-pop.
# The listener receives two R-TAG copies per request; the shared recover passes
# exactly one and discards the other.  The recovered plain ICMP reaches bond1's
# IP stack and a reply is sent, making ping succeed.
#
# Pass criteria:
#   - ping succeeds (rc=0)
#   - tcpdump on bond1 captures exactly PING_COUNT ICMP echo-request frames
#     (filter is restricted to type=8 to exclude echo replies, which would
#     double the count since bond1 also originates the reply packets)
#   - tc stats on veth_a1: passed >= PING_COUNT, discarded >= PING_COUNT
# ----------------------------------------------------------------------------
test_shared_recover_bond()
{
	local pcap cap_count ping_rc=0
	local dump_a
	local total_passed total_discarded tagless
	local result="pass"

	setup_push_mirror

	# veth_a1 ingress: create shared recover action with tag-pop
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_A1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_A1" ingress \
		protocol all flower skip_hw \
		action frer recover alg vector history-length 16 \
			reset-time 2000 tag-pop index $IDX_SHARED_RCVY

	# veth_b1 ingress: bind to the same shared action by index
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_B1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_B1" ingress \
		protocol all flower skip_hw \
		action frer recover index $IDX_SHARED_RCVY

	pcap=$(mktemp /tmp/frer_bond_shared_XXXXXX.pcap)
	capture_start "$pcap" "icmp[icmptype] == icmp-echo"

	do_ping || ping_rc=$?

	capture_stop

	cap_count=$(capture_count "$pcap")
	rm -f "$pcap"

	dump_a=$(ip netns exec "$NS_LISTENER" \
		$TC -s filter show dev "$VETH_A1" ingress 2>/dev/null)

	teardown_tc

	total_passed=$(tc_stat    "$dump_a" "passed")
	total_discarded=$(tc_stat "$dump_a" "discarded")
	tagless=$(tc_stat         "$dump_a" "tagless")
	total_discarded=$((total_discarded - tagless))

	echo "# bond shared recover: ping_rc=$ping_rc cap=$cap_count" \
		"passed=$total_passed discarded=$total_discarded"

	[ "$ping_rc"         -eq 0 ]            || result="fail"
	[ "$cap_count"       -eq "$PING_COUNT" ] || result="fail"
	[ "$total_passed"    -ge "$PING_COUNT" ] || result="fail"
	[ "$total_discarded" -ge "$PING_COUNT" ] || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"bond shared recover: ping OK, cap=$cap_count" \
			"passed=$total_passed discarded=$total_discarded"
	else
		ksft_test_result_fail \
			"bond shared recover: ping_rc=$ping_rc cap=$cap_count" \
			"passed=$total_passed discarded=$total_discarded" \
			"(expected ping OK, cap=$PING_COUNT," \
			"passed>=$PING_COUNT, discarded>=$PING_COUNT)"
	fi
}

# ----------------------------------------------------------------------------
# TEST 3: INDIVIDUAL RECOVER (bond topology)
#
# veth_a1 and veth_b1 use independent recover actions (idx=20 and idx=21).
# Each port maintains its own sequence history so both copies of every frame
# are passed (no cross-port deduplication).  With active-backup bond1, only
# the active slave's (veth_a1) recovered frame reaches bond1's IP stack, so
# ping succeeds.  The absence of deduplication is verified via per-slave
# tcpdump (each slave should capture PING_COUNT ICMP frames) and tc stats.
#
# Pass criteria:
#   - ping succeeds
#   - veth_a1 captures PING_COUNT ICMP frames (passed, not discarded)
#   - veth_b1 captures PING_COUNT ICMP frames (passed independently)
#   - tc stats: veth_a1 passed=PING_COUNT discarded=0
#               veth_b1 passed=PING_COUNT discarded=0
# ----------------------------------------------------------------------------
test_individual_recover_bond()
{
	local pcap_a pcap_b cap_a cap_b ping_rc=0
	local dump_a dump_b
	local passed_a discarded_a passed_b discarded_b tagless_a tagless_b
	local result="pass"

	setup_push_mirror

	# veth_a1 ingress: individual recover idx=20 (independent state)
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_A1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_A1" ingress \
		protocol all flower skip_hw \
		action frer recover individual alg vector history-length 16 \
			reset-time 2000 tag-pop index $IDX_INDV_RCVY_A

	# veth_b1 ingress: individual recover idx=21 (separate independent state)
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_B1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_B1" ingress \
		protocol all flower skip_hw \
		action frer recover individual alg vector history-length 16 \
			reset-time 2000 tag-pop index $IDX_INDV_RCVY_B

	# Per-slave capture A: verify veth_a1 passes frames; also use this run
	# for the overall ping_rc check (do_ping targets bond0->bond1).
	pcap_a=$(mktemp /tmp/frer_bond_indv_a_XXXXXX.pcap)
	capture_start_on "$NS_LISTENER" "$VETH_A1" "$pcap_a" "icmp"
	do_ping || ping_rc=$?
	capture_stop
	cap_a=$(capture_count_on "$NS_LISTENER" "$pcap_a")
	rm -f "$pcap_a"

	# Per-slave capture B: verify veth_b1 also passes frames (balance-rr
	# distributes egress across both slaves, so both paths carry traffic).
	pcap_b=$(mktemp /tmp/frer_bond_indv_b_XXXXXX.pcap)
	capture_start_on "$NS_LISTENER" "$VETH_B1" "$pcap_b" "icmp"
	do_ping || true
	capture_stop
	cap_b=$(capture_count_on "$NS_LISTENER" "$pcap_b")
	rm -f "$pcap_b"

	dump_a=$(ip netns exec "$NS_LISTENER" \
		$TC -s filter show dev "$VETH_A1" ingress 2>/dev/null)
	dump_b=$(ip netns exec "$NS_LISTENER" \
		$TC -s filter show dev "$VETH_B1" ingress 2>/dev/null)

	teardown_tc

	passed_a=$(tc_stat    "$dump_a" "passed")
	discarded_a=$(tc_stat "$dump_a" "discarded")
	tagless_a=$(tc_stat   "$dump_a" "tagless")
	passed_b=$(tc_stat    "$dump_b" "passed")
	discarded_b=$(tc_stat "$dump_b" "discarded")
	tagless_b=$(tc_stat   "$dump_b" "tagless")
	discarded_a=$((discarded_a - tagless_a))
	discarded_b=$((discarded_b - tagless_b))

	echo "# bond individual recover: ping_rc=$ping_rc" \
		"a1: cap=$cap_a passed=$passed_a discarded=$discarded_a" \
		"b1: cap=$cap_b passed=$passed_b discarded=$discarded_b"

	[ "$ping_rc"   -eq 0 ]            || result="fail"
	[ "$cap_a"     -ge "$PING_COUNT" ] || result="fail"
	[ "$cap_b"     -ge "$PING_COUNT" ] || result="fail"
	[ "$passed_a"  -ge "$PING_COUNT" ] || result="fail"
	[ "$passed_b"  -ge "$PING_COUNT" ] || result="fail"
	[ "$discarded_a" -eq 0 ]           || result="fail"
	[ "$discarded_b" -eq 0 ]           || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"bond individual recover: ping OK" \
			"a1: cap=$cap_a passed=$passed_a/0" \
			"b1: cap=$cap_b passed=$passed_b/0"
	else
		ksft_test_result_fail \
			"bond individual recover: ping_rc=$ping_rc" \
			"a1: cap=$cap_a passed=$passed_a discarded=$discarded_a" \
			"b1: cap=$cap_b passed=$passed_b discarded=$discarded_b"
	fi
}

# ----------------------------------------------------------------------------
# TEST 4: NO TAG-POP (bond topology)
#
# Shared recover runs without tag-pop; passed frames still carry the R-TAG
# when they reach bond1.
#
# Pass criteria:
#   - tcpdump on bond1 with "ether proto 0xf1c1" captures >= 1 R-TAG frame
#   - tcpdump on bond1 with "icmp" captures 0 frames (outer EtherType is
#     0xF1C1, not 0x0800, so plain-IP ICMP filter does not match)
# ----------------------------------------------------------------------------
test_no_tag_pop_bond()
{
	local pcap_rtag pcap_icmp rtag_count icmp_count
	local result="pass"

	setup_push_mirror

	# veth_a1 ingress: shared recover WITHOUT tag-pop
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_A1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_A1" ingress \
		protocol all flower skip_hw \
		action frer recover alg vector history-length 16 \
			reset-time 2000 index $IDX_NO_POP

	# veth_b1 ingress: bind to the same shared action
	ip netns exec "$NS_LISTENER" $TC qdisc add dev "$VETH_B1" clsact
	ip netns exec "$NS_LISTENER" $TC filter add dev "$VETH_B1" ingress \
		protocol all flower skip_hw \
		action frer recover index $IDX_NO_POP

	# Capture 1: frames with R-TAG EtherType on bond1 (expect >= 1)
	pcap_rtag=$(mktemp /tmp/frer_bond_nopop_rtag_XXXXXX.pcap)
	capture_start "$pcap_rtag" "ether proto 0xf1c1"
	ip netns exec "$NS_TALKER" \
		$PING -c 3 -W 1 -i 0.2 -q "$IP_DST" >/dev/null 2>&1 || true
	capture_stop
	rtag_count=$(capture_count "$pcap_rtag")
	rm -f "$pcap_rtag"

	# Capture 2: plain ICMP frames on bond1 (expect 0)
	pcap_icmp=$(mktemp /tmp/frer_bond_nopop_icmp_XXXXXX.pcap)
	capture_start "$pcap_icmp" "icmp"
	ip netns exec "$NS_TALKER" \
		$PING -c 3 -W 1 -i 0.2 -q "$IP_DST" >/dev/null 2>&1 || true
	capture_stop
	icmp_count=$(capture_count "$pcap_icmp")
	rm -f "$pcap_icmp"

	teardown_tc

	echo "# bond no tag-pop: rtag=$rtag_count (expected >=1) icmp=$icmp_count (expected 0)"

	[ "$rtag_count" -ge 1 ] || result="fail"
	[ "$icmp_count" -eq 0 ] || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"bond no tag-pop: R-TAG present on bond1 " \
			"(rtag=$rtag_count), ICMP absent (icmp=$icmp_count)"
	else
		ksft_test_result_fail \
			"bond no tag-pop: rtag=$rtag_count icmp=$icmp_count " \
			"(expected rtag>=1 icmp=0)"
	fi
}

# ----------------------------------------------------------------------------
# TEST 5: SIMPLE POINT-TO-POINT (no bond)
#
# Self-contained single-path topology: push on p2p_a0 egress, individual
# recover (with tag-pop) on p2p_a1 ingress.  IP is assigned directly to the
# veth interfaces (no bond).
#
# Pass criteria:
#   - ping succeeds (rc=0)
#   - veth_a1 recover stats: passed >= PING_COUNT, discarded = 0
# ----------------------------------------------------------------------------
test_simple_point_to_point()
{
	local ping_rc=0
	local dump_a1 passed discarded
	local result="pass"

	# Create self-contained p2p namespaces
	ip netns add "$P2P_NS_SRC"
	ip netns add "$P2P_NS_DST"

	ip link add "$P2P_VETH_A0" type veth peer name "$P2P_VETH_A1"
	ip link set "$P2P_VETH_A0" netns "$P2P_NS_SRC"
	ip link set "$P2P_VETH_A1" netns "$P2P_NS_DST"

	ip netns exec "$P2P_NS_SRC" ip link set lo up
	ip netns exec "$P2P_NS_SRC" ip link set "$P2P_VETH_A0" up
	ip netns exec "$P2P_NS_SRC" ip addr add "${IP_P2P_SRC}/24" dev "$P2P_VETH_A0"

	ip netns exec "$P2P_NS_DST" ip link set lo up
	ip netns exec "$P2P_NS_DST" ip link set "$P2P_VETH_A1" up
	ip netns exec "$P2P_NS_DST" ip addr add "${IP_P2P_DST}/24" dev "$P2P_VETH_A1"

	local mac_a0 mac_a1
	mac_a0=$(ip netns exec "$P2P_NS_SRC" cat /sys/class/net/"$P2P_VETH_A0"/address)
	mac_a1=$(ip netns exec "$P2P_NS_DST" cat /sys/class/net/"$P2P_VETH_A1"/address)
	ip netns exec "$P2P_NS_SRC" ip neigh add "$IP_P2P_DST" lladdr "$mac_a1" dev "$P2P_VETH_A0"
	ip netns exec "$P2P_NS_DST" ip neigh add "$IP_P2P_SRC" lladdr "$mac_a0" dev "$P2P_VETH_A1"

	# veth_a0 egress: push R-TAG
	ip netns exec "$P2P_NS_SRC" $TC qdisc add dev "$P2P_VETH_A0" clsact
	ip netns exec "$P2P_NS_SRC" $TC filter add dev "$P2P_VETH_A0" egress \
		protocol ip flower skip_hw \
		action frer push index $IDX_PUSH

	# veth_a1 ingress: individual recover with tag-pop
	ip netns exec "$P2P_NS_DST" $TC qdisc add dev "$P2P_VETH_A1" clsact
	ip netns exec "$P2P_NS_DST" $TC filter add dev "$P2P_VETH_A1" ingress \
		protocol all flower skip_hw \
		action frer recover individual alg vector history-length 16 \
			reset-time 2000 tag-pop index $IDX_P2P_RCVY

	ip netns exec "$P2P_NS_SRC" \
		$PING -c "$PING_COUNT" -W "$PING_TIMEOUT" -i 0.2 -q \
		"$IP_P2P_DST" >/dev/null 2>&1 || ping_rc=$?

	dump_a1=$(ip netns exec "$P2P_NS_DST" \
		$TC -s filter show dev "$P2P_VETH_A1" ingress 2>/dev/null)

	# Teardown p2p topology
	for dev in "$P2P_VETH_A0"; do
		ip netns exec "$P2P_NS_SRC" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	for dev in "$P2P_VETH_A1"; do
		ip netns exec "$P2P_NS_DST" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	ip netns exec "$P2P_NS_SRC" $TC actions flush action frer 2>/dev/null || true
	ip netns exec "$P2P_NS_DST" $TC actions flush action frer 2>/dev/null || true
	ip netns del "$P2P_NS_SRC" 2>/dev/null || true
	ip netns del "$P2P_NS_DST" 2>/dev/null || true

	passed=$(tc_stat    "$dump_a1" "passed")
	discarded=$(tc_stat "$dump_a1" "discarded")
	local tagless
	tagless=$(tc_stat   "$dump_a1" "tagless")
	discarded=$((discarded - tagless))

	echo "# p2p: ping_rc=$ping_rc passed=$passed discarded=$discarded"

	[ "$ping_rc"   -eq 0 ]            || result="fail"
	[ "$passed"    -ge "$PING_COUNT" ] || result="fail"
	[ "$discarded" -eq 0 ]            || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"simple p2p: ping OK, passed=$passed discarded=$discarded"
	else
		ksft_test_result_fail \
			"simple p2p: ping_rc=$ping_rc passed=$passed discarded=$discarded"
	fi
}

# ----------------------------------------------------------------------------
# TEST 6: RELAY E2E (self-contained, no bond)
#
# Talker sends VLAN-100 frames into bridge0 (sequence generator).  Bridge0
# pushes an R-TAG and replicates to two redundant paths.  Bridge1 (eliminator)
# recovers (shared, tag-pop) on both paths and forwards the deduplicated frame
# to the listener.
#
# Topology:
#   ns_talker (talker_eth.100) -- talker_eth/br0_uplink
#       -- bridge0 (br_r0) -+- br0_swp0/br1_swp0 -+
#                            \- br0_swp1/br1_swp1 -+
#       -- bridge1 (br_r1) -- br1_downlink/listener_eth -- ns_listener
#
# FRER rules:
#   bridge0 / br0_uplink ingress  : push idx=50, redirect br0_swp0, mirror br0_swp1
#   bridge1 / br1_swp0 ingress    : recover (shared, tag-pop) idx=60, redirect br1_downlink
#   bridge1 / br1_swp1 ingress    : recover idx=60 (bind same), redirect br1_downlink
#   bridge1 / br1_downlink ingress: redirect br1_swp0 (reply path, bypass FDB)
#
# Pass criteria:
#   - ping from ns_talker to ns_listener succeeds (rc=0)
#   - tcpdump on listener captures exactly PING_COUNT ICMP echo-request frames
#   - br1_swp0 tc stats: passed >= PING_COUNT, discarded >= PING_COUNT
# ----------------------------------------------------------------------------
teardown_relay_tc()
{
	for dev in "$R_BR0_UPLINK"; do
		ip netns exec "$R_NS_BRIDGE0" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	for dev in "$R_BR1_SWP0" "$R_BR1_SWP1" "$R_BR1_DOWNLINK"; do
		ip netns exec "$R_NS_BRIDGE1" $TC qdisc del dev "$dev" clsact \
			2>/dev/null || true
	done
	ip netns exec "$R_NS_BRIDGE0" $TC actions flush action frer 2>/dev/null || true
	ip netns exec "$R_NS_BRIDGE1" $TC actions flush action frer 2>/dev/null || true
}

test_relay_e2e()
{
	local ping_rc=0
	local dump_r1swp0
	local total_passed total_discarded
	local result="pass"
	local ns

	for ns in "$R_NS_TALKER" "$R_NS_BRIDGE0" "$R_NS_BRIDGE1" "$R_NS_LISTENER"; do
		ip netns add "$ns" || {
			echo "# relay e2e: failed to create netns $ns" >&2
			ksft_test_result_skip "relay e2e: netns setup failed"
			return
		}
	done

	ip link add "$R_TALKER_ETH"   type veth peer name "$R_BR0_UPLINK"
	ip link add "$R_BR0_SWP0"     type veth peer name "$R_BR1_SWP0"
	ip link add "$R_BR0_SWP1"     type veth peer name "$R_BR1_SWP1"
	ip link add "$R_BR1_DOWNLINK" type veth peer name "$R_LISTENER_ETH"

	ip link set "$R_TALKER_ETH"   netns "$R_NS_TALKER"
	ip link set "$R_BR0_UPLINK"   netns "$R_NS_BRIDGE0"
	ip link set "$R_BR0_SWP0"     netns "$R_NS_BRIDGE0"
	ip link set "$R_BR0_SWP1"     netns "$R_NS_BRIDGE0"
	ip link set "$R_BR1_SWP0"     netns "$R_NS_BRIDGE1"
	ip link set "$R_BR1_SWP1"     netns "$R_NS_BRIDGE1"
	ip link set "$R_BR1_DOWNLINK" netns "$R_NS_BRIDGE1"
	ip link set "$R_LISTENER_ETH" netns "$R_NS_LISTENER"

	local ns_dev
	for ns_dev in \
		"$R_NS_TALKER:$R_TALKER_ETH" \
		"$R_NS_BRIDGE0:$R_BR0_UPLINK" "$R_NS_BRIDGE0:$R_BR0_SWP0" \
		"$R_NS_BRIDGE0:$R_BR0_SWP1" \
		"$R_NS_BRIDGE1:$R_BR1_SWP0" "$R_NS_BRIDGE1:$R_BR1_SWP1" \
		"$R_NS_BRIDGE1:$R_BR1_DOWNLINK" \
		"$R_NS_LISTENER:$R_LISTENER_ETH"; do
		local _ns="${ns_dev%%:*}"
		local _dev="${ns_dev##*:}"
		ip netns exec "$_ns" ip link set lo up
		ip netns exec "$_ns" ip link set "$_dev" up
	done

	# bridge0: sequence generator, VLAN filtering
	ip netns exec "$R_NS_BRIDGE0" ip link add name "$R_BR0" type bridge vlan_filtering 1
	ip netns exec "$R_NS_BRIDGE0" ip link set "$R_BR0" up
	ip netns exec "$R_NS_BRIDGE0" ip link set "$R_BR0_UPLINK" master "$R_BR0"
	ip netns exec "$R_NS_BRIDGE0" ip link set "$R_BR0_SWP0" master "$R_BR0"
	ip netns exec "$R_NS_BRIDGE0" ip link set "$R_BR0_SWP1" master "$R_BR0"

	ip netns exec "$R_NS_BRIDGE0" bridge vlan add dev "$R_BR0_UPLINK" vid "$R_VLAN"
	ip netns exec "$R_NS_BRIDGE0" bridge vlan add dev "$R_BR0_SWP0" vid "$R_VLAN"
	ip netns exec "$R_NS_BRIDGE0" bridge vlan del dev "$R_BR0_SWP1" vid 1
	ip netns exec "$R_NS_BRIDGE0" bridge vlan add dev "$R_BR0_SWP1" \
		vid "$R_VLAN" pvid untagged
	ip netns exec "$R_NS_BRIDGE0" bridge link set dev "$R_BR0_SWP0" learning off
	ip netns exec "$R_NS_BRIDGE0" bridge link set dev "$R_BR0_SWP1" learning off
	ip netns exec "$R_NS_BRIDGE0" bridge vlan set dev "$R_BR0_SWP0" vid "$R_VLAN" noflood
	ip netns exec "$R_NS_BRIDGE0" bridge vlan set dev "$R_BR0_SWP1" vid "$R_VLAN" noflood

	# bridge1: eliminator, VLAN filtering
	ip netns exec "$R_NS_BRIDGE1" ip link add name "$R_BR1" type bridge vlan_filtering 1
	ip netns exec "$R_NS_BRIDGE1" ip link set "$R_BR1" up
	ip netns exec "$R_NS_BRIDGE1" ip link set "$R_BR1_SWP0" master "$R_BR1"
	ip netns exec "$R_NS_BRIDGE1" ip link set "$R_BR1_SWP1" master "$R_BR1"
	ip netns exec "$R_NS_BRIDGE1" ip link set "$R_BR1_DOWNLINK" master "$R_BR1"

	ip netns exec "$R_NS_BRIDGE1" bridge vlan add dev "$R_BR1_SWP0" vid "$R_VLAN"
	ip netns exec "$R_NS_BRIDGE1" bridge vlan del dev "$R_BR1_SWP1" vid 1
	ip netns exec "$R_NS_BRIDGE1" bridge vlan add dev "$R_BR1_SWP1" \
		vid "$R_VLAN" pvid untagged
	ip netns exec "$R_NS_BRIDGE1" bridge vlan add dev "$R_BR1_DOWNLINK" vid "$R_VLAN"
	ip netns exec "$R_NS_BRIDGE1" bridge link set dev "$R_BR1_SWP0" learning off
	ip netns exec "$R_NS_BRIDGE1" bridge link set dev "$R_BR1_SWP1" learning off
	ip netns exec "$R_NS_BRIDGE1" bridge vlan set dev "$R_BR1_SWP0" vid "$R_VLAN" noflood
	ip netns exec "$R_NS_BRIDGE1" bridge vlan set dev "$R_BR1_SWP1" vid "$R_VLAN" noflood

	# ns_talker: VLAN sub-interface
	ip netns exec "$R_NS_TALKER" ip link add link "$R_TALKER_ETH" \
		name "${R_TALKER_ETH}.${R_VLAN}" type vlan id "$R_VLAN"
	ip netns exec "$R_NS_TALKER" ip link set "${R_TALKER_ETH}.${R_VLAN}" up
	ip netns exec "$R_NS_TALKER" ip addr add "${R_IP_TALKER}/24" \
		dev "${R_TALKER_ETH}.${R_VLAN}"

	# ns_listener: VLAN sub-interface
	ip netns exec "$R_NS_LISTENER" ip link add link "$R_LISTENER_ETH" \
		name "${R_LISTENER_ETH}.${R_VLAN}" type vlan id "$R_VLAN"
	ip netns exec "$R_NS_LISTENER" ip link set "${R_LISTENER_ETH}.${R_VLAN}" up
	ip netns exec "$R_NS_LISTENER" ip addr add "${R_IP_LISTENER}/24" \
		dev "${R_LISTENER_ETH}.${R_VLAN}"

	# Static ARP (VLAN 100 flooding is disabled)
	local mac_talker mac_listener
	mac_talker=$(ip netns exec "$R_NS_TALKER" \
		cat /sys/class/net/"${R_TALKER_ETH}.${R_VLAN}"/address)
	mac_listener=$(ip netns exec "$R_NS_LISTENER" \
		cat /sys/class/net/"${R_LISTENER_ETH}.${R_VLAN}"/address)
	ip netns exec "$R_NS_TALKER"   ip neigh add "$R_IP_LISTENER" \
		lladdr "$mac_listener" dev "${R_TALKER_ETH}.${R_VLAN}"
	ip netns exec "$R_NS_LISTENER" ip neigh add "$R_IP_TALKER" \
		lladdr "$mac_talker"   dev "${R_LISTENER_ETH}.${R_VLAN}"

	# bridge0 / br0_uplink ingress: push R-TAG then replicate to both redundant paths.
	# mirror must come before redirect because redirect is a terminating action.
	ip netns exec "$R_NS_BRIDGE0" $TC qdisc add dev "$R_BR0_UPLINK" clsact
	ip netns exec "$R_NS_BRIDGE0" $TC filter add dev "$R_BR0_UPLINK" ingress \
		protocol 802.1Q flower skip_hw vlan_id "$R_VLAN" \
		action frer push index $IDX_RELAY_PUSH \
		action mirred egress mirror  dev "$R_BR0_SWP1" \
		action mirred egress redirect dev "$R_BR0_SWP0"

	# bridge1 / br1_swp0 ingress: create shared recover action (tag-pop)
	ip netns exec "$R_NS_BRIDGE1" $TC qdisc add dev "$R_BR1_SWP0" clsact
	ip netns exec "$R_NS_BRIDGE1" $TC filter add dev "$R_BR1_SWP0" ingress \
		protocol all flower skip_hw \
		action frer recover alg vector history-length 16 \
			reset-time 2000 tag-pop index $IDX_RELAY_RCVY \
		action mirred egress redirect dev "$R_BR1_DOWNLINK"

	# bridge1 / br1_swp1 ingress: bind to the same shared recover action
	ip netns exec "$R_NS_BRIDGE1" $TC qdisc add dev "$R_BR1_SWP1" clsact
	ip netns exec "$R_NS_BRIDGE1" $TC filter add dev "$R_BR1_SWP1" ingress \
		protocol all flower skip_hw \
		action frer recover index $IDX_RELAY_RCVY \
		action mirred egress redirect dev "$R_BR1_DOWNLINK"

	# bridge1 / br1_downlink ingress: redirect VLAN 100 replies directly to br1_swp0
	ip netns exec "$R_NS_BRIDGE1" $TC qdisc add dev "$R_BR1_DOWNLINK" clsact
	ip netns exec "$R_NS_BRIDGE1" $TC filter add dev "$R_BR1_DOWNLINK" ingress \
		protocol 802.1Q flower skip_hw vlan_id "$R_VLAN" \
		action mirred egress redirect dev "$R_BR1_SWP0"

	# Capture ICMP echo-requests on listener_eth.VLAN to verify exactly
	# PING_COUNT deduplicated frames reach the listener after recovery.
	local pcap cap_count
	pcap=$(mktemp /tmp/frer_relay_XXXXXX.pcap)
	capture_start_on "$R_NS_LISTENER" "${R_LISTENER_ETH}.${R_VLAN}" \
		"$pcap" "icmp[icmptype] == icmp-echo"

	ip netns exec "$R_NS_TALKER" \
		$PING -c "$PING_COUNT" -W "$PING_TIMEOUT" -i 0.2 -q \
		"$R_IP_LISTENER" >/dev/null 2>&1 || ping_rc=$?

	capture_stop
	cap_count=$(capture_count_on "$R_NS_LISTENER" "$pcap")
	rm -f "$pcap"

	dump_br1_swp0=$(ip netns exec "$R_NS_BRIDGE1" \
		$TC -s filter show dev "$R_BR1_SWP0" ingress 2>/dev/null)

	teardown_relay_tc
	for ns in "$R_NS_TALKER" "$R_NS_BRIDGE0" "$R_NS_BRIDGE1" "$R_NS_LISTENER"; do
		ip netns del "$ns" 2>/dev/null || true
	done

	total_passed=$(tc_stat    "$dump_br1_swp0" "passed")
	total_discarded=$(tc_stat "$dump_br1_swp0" "discarded")
	local tagless
	tagless=$(tc_stat         "$dump_br1_swp0" "tagless")
	total_discarded=$((total_discarded - tagless))

	echo "# relay e2e: ping_rc=$ping_rc cap=$cap_count" \
		"passed=$total_passed discarded=$total_discarded"

	[ "$ping_rc"         -eq 0 ]            || result="fail"
	[ "$cap_count"       -eq "$PING_COUNT" ] || result="fail"
	[ "$total_passed"    -ge "$PING_COUNT" ] || result="fail"
	[ "$total_discarded" -ge "$PING_COUNT" ] || result="fail"

	if [ "$result" = "pass" ]; then
		ksft_test_result_pass \
			"relay e2e: ping OK, cap=$cap_count " \
			"passed=$total_passed discarded=$total_discarded"
	else
		ksft_test_result_fail \
			"relay e2e: ping_rc=$ping_rc cap=$cap_count " \
			"passed=$total_passed discarded=$total_discarded" \
			"(expected ping OK, cap=$PING_COUNT," \
			"passed>=$PING_COUNT, discarded>=$PING_COUNT)"
	fi
}

# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
main()
{
	ksft_print_header
	check_prerequisites
	load_module
	setup_topology

	if ! check_frer_action; then
		ksft_set_plan "$NUM_TESTS"
		for i in $(seq 1 "$NUM_TESTS"); do
			ksft_test_result_skip \
				"frer action not available in this kernel (test $i)"
		done
		ksft_print_cnts
		exit "$KSFT_SKIP"
	fi

	ksft_set_plan "$NUM_TESTS"

	test_push_verify_bond        # TEST 1: push on a0/b0, no recover, R-TAG on both paths
	test_shared_recover_bond     # TEST 2: shared recover, dedup, ping succeeds
	test_individual_recover_bond # TEST 3: individual recover, no dedup, double frames
	test_no_tag_pop_bond         # TEST 4: shared recover without tag-pop, R-TAG preserved
	test_simple_point_to_point   # TEST 5: single-path p2p, no bond
	test_relay_e2e               # TEST 6: relay bridge topology

	ksft_print_cnts

	[ "$_ksft_fail" -eq 0 ] && ksft_exit_pass || ksft_exit_fail
}

main "$@"
