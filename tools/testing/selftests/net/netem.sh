#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test netem qdisc focusing on:
#   - valid multi-netem trees (HTB, HFSC parents)
#   - packet duplication under various topologies
#   - robustness against misconfigurations that previously caused
#     soft lockups, OOM, or UAF (syzkaller-class inputs)
#
# These tests do NOT validate statistical accuracy of delay/loss
# distributions; they verify the kernel doesn't crash and that
# packets actually flow (or are cleanly dropped) under each config.
#
# Author: Stephen Hemminger <stephen@networkplumber.org>

source lib.sh

NSPREFIX="netem-test"
NS_SRC="${NSPREFIX}-src"
NS_DST="${NSPREFIX}-dst"

VETH_SRC="veth-src"
VETH_DST="veth-dst"

SRC_IP="192.168.99.1"
DST_IP="192.168.99.2"

# How long to wait for a potentially-stuck kernel (seconds)
LOCKUP_TIMEOUT=15

PASS=0
FAIL=0
SKIP=0

cleanup()
{
	ip netns del "$NS_SRC" 2>/dev/null
	ip netns del "$NS_DST" 2>/dev/null
}

setup()
{
	cleanup

	ip netns add "$NS_SRC" || return $ksft_skip
	ip netns add "$NS_DST" || return $ksft_skip

	ip -n "$NS_SRC" link add "$VETH_SRC" type veth \
		peer name "$VETH_DST" netns "$NS_DST" || return $ksft_skip

	ip -n "$NS_SRC" addr add "${SRC_IP}/24" dev "$VETH_SRC"
	ip -n "$NS_DST" addr add "${DST_IP}/24" dev "$VETH_DST"

	ip -n "$NS_SRC" link set "$VETH_SRC" up
	ip -n "$NS_DST" link set "$VETH_DST" up

	ip -n "$NS_SRC" link set lo up
	ip -n "$NS_DST" link set lo up

	# Sanity: can we ping at all?
	ip netns exec "$NS_SRC" ping -c 1 -W 2 "$DST_IP" >/dev/null 2>&1
	if [ $? -ne 0 ]; then
		echo "# SKIP: basic connectivity failed"
		return $ksft_skip
	fi

	return 0
}

# Reset qdisc on src veth to default (remove any previous test config)
reset_qdisc()
{
	# Remove any existing root qdisc. Must use 'tc' not 'ip'.
	run_tc qdisc del dev "$VETH_SRC" root 2>/dev/null
	return 0
}

# Send ICMP ping traffic and require at least one reply.
# Uses generous intervals to accommodate delay/loss configs.
# Usage: send_traffic <count> [timeout]
send_traffic()
{
	local count=${1:-20}
	local timeout=${2:-10}
	local received

	received=$(ip netns exec "$NS_SRC" \
		timeout "$timeout" \
		ping -c "$count" -i 0.1 -W 2 "$DST_IP" 2>/dev/null |
		awk '/packets transmitted/ { print $4 }')

	[ "${received:-0}" -gt 0 ] 2>/dev/null
}

# Send traffic and verify kernel didn't lock up.
# Returns 0 if kernel survived (regardless of whether traffic arrived).
# Usage: survive_traffic <count> [timeout]
survive_traffic()
{
	local count=${1:-20}
	local timeout=${2:-$LOCKUP_TIMEOUT}

	# Use ping as the traffic source — simpler and more reliable
	# for crash testing. We don't care about replies.
	ip netns exec "$NS_SRC" \
		timeout "$timeout" ping -c "$count" -i 0.05 -W 1 "$DST_IP" \
		>/dev/null 2>&1
	local rc=$?

	# timeout returns 124 if it had to kill the child
	if [ $rc -eq 124 ]; then
		return 1  # kernel likely stuck
	fi

	return 0
}

log_result()
{
	local result=$1
	local name=$2

	case $result in
	0)
		printf "    PASS: %s\n" "$name"
		PASS=$((PASS + 1))
		;;
	$ksft_skip)
		printf "    SKIP: %s\n" "$name"
		SKIP=$((SKIP + 1))
		;;
	*)
		printf "    FAIL: %s\n" "$name"
		FAIL=$((FAIL + 1))
		ret=1
		;;
	esac
}

run_cmd()
{
	ip netns exec "$NS_SRC" "$@"
}

run_tc()
{
	ip netns exec "$NS_SRC" tc "$@"
}

# =====================================================================
# TEST CASES
# =====================================================================

# --- Group 1: Basic sanity ---

test_basic_delay()
{
	local name="basic netem delay"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 10ms || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_basic_duplicate()
{
	local name="basic netem duplicate 50%"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 10ms duplicate 50% || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_basic_loss()
{
	local name="basic netem loss 30%"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 5ms loss 30% || {
		log_result 1 "$name"
		return
	}

	# With 30% loss, 20 packets should still get some through
	if send_traffic 40; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_basic_corrupt()
{
	local name="basic netem corrupt 10%"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 5ms corrupt 10% || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_basic_reorder()
{
	local name="basic netem reorder 25% gap 5"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 50ms \
		reorder 25% 50% gap 5 || {
		log_result 1 "$name"
		return
	}

	if send_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_pfifo_child()
{
	local name="netem with pfifo child qdisc"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: \
		netem delay 10ms 50ms || {
		log_result 1 "$name"
		return
	}
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 \
		pfifo limit 1000 || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

# --- Group 2: Multi-netem trees (the configs that check_netem_in_tree blocked) ---

test_htb_two_netem_leaves()
{
	local name="HTB root, two netem leaves (no dup)"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: htb default 10 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:10 \
		htb rate 10mbit || {
		log_result 1 "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:20 \
		htb rate 10mbit || {
		log_result 1 "$name"
		return
	}
	run_tc qdisc add dev "$VETH_SRC" parent 1:10 handle 10: \
		netem delay 10ms || {
		log_result 1 "$name"
		return
	}
	run_tc qdisc add dev "$VETH_SRC" parent 1:20 handle 20: \
		netem delay 20ms || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_htb_netem_dup_one_leaf()
{
	local name="HTB root, one netem with dup, one without"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: htb default 10 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:10 \
		htb rate 10mbit
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:20 \
		htb rate 10mbit

	# First leaf: netem with duplication
	run_tc qdisc add dev "$VETH_SRC" parent 1:10 handle 10: \
		netem delay 10ms duplicate 25% 2>/dev/null
	local rc1=$?

	# Second leaf: netem without duplication
	run_tc qdisc add dev "$VETH_SRC" parent 1:20 handle 20: \
		netem delay 20ms 2>/dev/null
	local rc2=$?

	# check_netem_in_tree rejects this valid config — that's a bug.
	if [ $rc1 -ne 0 ] || [ $rc2 -ne 0 ]; then
		echo "# tc rejected multi-netem tree (check_netem_in_tree bug)"
		log_result 1 "$name"
		return
	fi

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_hfsc_netem_child()
{
	local name="HFSC root, netem child (CVE-2025-37890 topology)"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: hfsc default 1 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:1 \
		hfsc ls rate 50mbit ul rate 50mbit || {
		log_result 1 "$name"
		return
	}
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		netem delay 10ms || {
		log_result 1 "$name"
		return
	}

	if send_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_hfsc_netem_dup()
{
	local name="HFSC root, netem child with duplicate"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: hfsc default 1 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:1 \
		hfsc ls rate 50mbit ul rate 50mbit
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		netem delay 10ms duplicate 30%

	if [ $? -ne 0 ]; then
		echo "# tc rejected HFSC + netem duplicate"
		log_result $ksft_skip "$name"
		return
	fi

	# This is the topology that triggered CVE-2025-37890.
	# We just need to survive it.
	if survive_traffic 50; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_hfsc_two_netem_classes()
{
	local name="HFSC root, two classes each with netem"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: hfsc default 1 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:1 \
		hfsc ls rate 25mbit ul rate 25mbit
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:2 \
		hfsc ls rate 25mbit ul rate 25mbit

	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		netem delay 10ms
	run_tc qdisc add dev "$VETH_SRC" parent 1:2 handle 20: \
		netem delay 20ms

	if [ $? -ne 0 ]; then
		log_result $ksft_skip "$name"
		return
	fi

	if send_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

# --- Group 3: Inner qdisc combinations ---

test_netem_tbf_child()
{
	local name="netem with TBF child (non-work-conserving)"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: \
		netem delay 10ms || {
		log_result $ksft_skip "$name"
		return
	}

	# TBF as inner qdisc — previously caused stalls with
	# some parent qdiscs. As a direct child of netem, the
	# expectation is packets flow (possibly rate-limited).
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		tbf rate 1mbit burst 10kb latency 50ms || {
		log_result $ksft_skip "$name"
		return
	}

	if survive_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_htb_netem_tbf_chain()
{
	local name="HTB -> netem -> TBF chain"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: htb default 10 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:10 \
		htb rate 5mbit
	run_tc qdisc add dev "$VETH_SRC" parent 1:10 handle 10: \
		netem delay 10ms
	run_tc qdisc add dev "$VETH_SRC" parent 10:1 handle 100: \
		tbf rate 1mbit burst 10kb latency 50ms

	if [ $? -ne 0 ]; then
		log_result $ksft_skip "$name"
		return
	fi

	# This is a historically problematic config. Just surviving is success.
	if survive_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_netem_sfq_child()
{
	local name="netem with SFQ child qdisc"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: \
		netem delay 10ms || {
		log_result 1 "$name"
		return
	}
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		sfq perturb 10 || {
		log_result $ksft_skip "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

# --- Group 4: Abuse / crash resistance ---

test_dup_100_percent()
{
	local name="netem duplicate 100% (stress)"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem delay 5ms duplicate 100% || {
		log_result 1 "$name"
		return
	}

	# 100% dup means every packet cloned. Must not explode.
	if survive_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_dup_100_no_delay()
{
	local name="netem duplicate 100% with zero delay"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem duplicate 100% || {
		log_result 1 "$name"
		return
	}

	# Zero delay + 100% dup is a degenerate case
	if survive_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_all_impairments()
{
	local name="netem all impairments simultaneously"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem \
		delay 10ms 5ms 25% \
		loss 10% \
		duplicate 10% \
		corrupt 5% \
		reorder 5% 50% || {
		log_result 1 "$name"
		return
	}

	if survive_traffic 50; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_rapid_reconfig()
{
	local name="rapid qdisc add/change/delete cycling"

	reset_qdisc

	# Rapidly add, change, and delete netem config while traffic runs.
	# This is the kind of thing syzkaller does.
	ip netns exec "$NS_SRC" ping -c 100 -i 0.02 -W 1 "$DST_IP" \
		>/dev/null 2>&1 &
	local traffic_pid=$!

	local i
	for i in $(seq 1 20); do
		run_tc qdisc replace dev "$VETH_SRC" root netem delay 5ms \
			duplicate 10% 2>/dev/null
		run_tc qdisc change dev "$VETH_SRC" root netem delay 10ms \
			loss 5% 2>/dev/null
		run_tc qdisc change dev "$VETH_SRC" root netem delay 1ms \
			duplicate 50% 2>/dev/null
		run_tc qdisc del dev "$VETH_SRC" root 2>/dev/null
	done

	wait $traffic_pid 2>/dev/null

	# If we got here, the kernel didn't lock up
	log_result 0 "$name"
}

test_limit_1()
{
	local name="netem limit 1 with duplication"

	reset_qdisc
	# Minimal queue limit with duplication — exercises the
	# t_len >= sch->limit drop path in enqueue
	run_tc qdisc replace dev "$VETH_SRC" root netem \
		limit 1 delay 10ms duplicate 50% || {
		log_result 1 "$name"
		return
	}

	# Most packets will be dropped, but kernel must survive
	if survive_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_slot_config()
{
	local name="netem slot configuration"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem \
		delay 10ms slot 20ms 40ms packets 5 bytes 1500 || {
		# slot may not be supported on old kernels
		log_result $ksft_skip "$name"
		return
	}

	if send_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_rate_limiting()
{
	local name="netem rate limiting"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root netem \
		delay 10ms rate 1mbit || {
		log_result 1 "$name"
		return
	}

	if send_traffic 20; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

# --- Group 5: Multi-netem dup trees (the CVE-2025-38553 triggers) ---

test_htb_two_netem_both_dup()
{
	local name="HTB root, two netem leaves both duplicating"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: htb default 10 || {
		log_result $ksft_skip "$name"
		return
	}
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:10 \
		htb rate 10mbit
	run_tc class add dev "$VETH_SRC" parent 1: classid 1:20 \
		htb rate 10mbit

	run_tc qdisc add dev "$VETH_SRC" parent 1:10 handle 10: \
		netem delay 10ms duplicate 25% 2>/dev/null
	local rc1=$?
	run_tc qdisc add dev "$VETH_SRC" parent 1:20 handle 20: \
		netem delay 10ms duplicate 25% 2>/dev/null
	local rc2=$?

	if [ $rc1 -ne 0 ] || [ $rc2 -ne 0 ]; then
		echo "# tc rejected multi-netem dup tree (check_netem_in_tree bug)"
		log_result 1 "$name"
		return
	fi

	# This was the CVE-2025-38553 scenario.
	# With recursion guard: should survive.
	# Without: may lock up — that's what the timeout catches.
	if survive_traffic 50 "$LOCKUP_TIMEOUT"; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

test_nested_netem()
{
	local name="nested netem (netem inside netem child)"

	reset_qdisc
	run_tc qdisc replace dev "$VETH_SRC" root handle 1: \
		netem delay 10ms || {
		log_result 1 "$name"
		return
	}
	# Try to add netem as child of netem. This is unusual
	# but shouldn't crash.
	run_tc qdisc add dev "$VETH_SRC" parent 1:1 handle 10: \
		netem delay 5ms duplicate 10% 2>/dev/null

	if [ $? -ne 0 ]; then
		echo "# nested netem rejected by kernel (check_netem_in_tree bug)"
		log_result 1 "$name"
		return
	fi

	if survive_traffic 30; then
		log_result 0 "$name"
	else
		log_result 1 "$name"
	fi
}

# =====================================================================
# MAIN
# =====================================================================

trap cleanup EXIT

ret=0

# Check prerequisites
if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: must be run as root"
	exit $ksft_skip
fi

for tool in tc ip ping; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIP: $tool not found"
		exit $ksft_skip
	fi
done

# Check kernel has netem
if ! ip netns add "${NSPREFIX}-probe" 2>/dev/null; then
	echo "SKIP: network namespaces not available"
	exit $ksft_skip
fi
ip netns exec "${NSPREFIX}-probe" \
	tc qdisc add dev lo root netem delay 1ms 2>/dev/null
if [ $? -ne 0 ]; then
	ip netns del "${NSPREFIX}-probe"
	echo "SKIP: netem qdisc not available (CONFIG_NET_SCH_NETEM)"
	exit $ksft_skip
fi
ip netns del "${NSPREFIX}-probe"

setup
if [ $? -eq $ksft_skip ]; then
	echo "SKIP: setup failed"
	exit $ksft_skip
fi

# Group 1: Basic sanity
test_basic_delay
test_basic_duplicate
test_basic_loss
test_basic_corrupt
test_basic_reorder
test_pfifo_child

# Group 2: Multi-netem trees
test_htb_two_netem_leaves
test_htb_netem_dup_one_leaf
test_hfsc_netem_child
test_hfsc_netem_dup
test_hfsc_two_netem_classes

# Group 3: Inner qdisc combos
test_netem_tbf_child
test_htb_netem_tbf_chain
test_netem_sfq_child

# Group 4: Abuse / crash resistance
test_dup_100_percent
test_dup_100_no_delay
test_all_impairments
test_rapid_reconfig
test_limit_1
test_slot_config
test_rate_limiting

# Group 5: Multi-netem dup (CVE triggers)
test_htb_two_netem_both_dup
test_nested_netem

echo
echo "Summary: $PASS pass, $FAIL fail, $SKIP skip"

exit $ret
