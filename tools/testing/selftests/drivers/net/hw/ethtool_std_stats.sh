#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#shellcheck disable=SC2034 # SC does not see the global variables
#shellcheck disable=SC2317,SC2329 # unused functions

ALL_TESTS="
	test_eth_ctrl_stats
	test_eth_mac_stats
	test_pause_stats
"
STABLE_MAC_ADDRS=yes
NUM_NETIFS=2
lib_dir=$(dirname "$0")
# shellcheck source=./../../../net/forwarding/lib.sh
source "$lib_dir"/../../../net/forwarding/lib.sh

UINT32_MAX=$((2**32 - 1))

traffic_test()
{
	local iface=$1; shift
	local neigh=$1; shift
	local num_tx=$1; shift
	local pkt_format="$1"; shift
	local -a counters=("$@")
	local int grp counter target exact_check
	local before after delta
	local num_rx=$((num_tx * 2))
	local xfail_message
	local src="aggregate"
	local i

	for i in "${!counters[@]}"; do
		read -r int grp counter target exact_check xfail_message <<< "${counters[$i]}"

		before[i]=$(ethtool_std_stats_get "$int" "$grp" "$counter" "$src")
	done

	# shellcheck disable=SC2086 # needs split options
	run_on "$iface" "$MZ" "$iface" -q -c "$num_tx" $pkt_format

	# shellcheck disable=SC2086 # needs split options
	run_on "$neigh" "$MZ" "$neigh" -q -c "$num_rx" $pkt_format

	for i in "${!counters[@]}"; do
		read -r int grp counter target exact_check xfail_message <<< "${counters[$i]}"

		after[i]=$(ethtool_std_stats_get "$int" "$grp" "$counter" "$src")
		if [[ "${after[$i]}" == "null" ]]; then
			log_test_skip "$grp-$counter on $int" "not supported"
			continue;
		fi

		delta=$((after[i] - before[i]))

		RET=0
		if [ "$exact_check" -ne 0 ]; then
			[ "$delta" -eq "$target" ]
		else
			[ "$delta" -ge "$target" ] && [ "$delta" -le "$UINT32_MAX" ]
		fi
		err="$?"

		if [[ $err != 0  ]] && [[ -n $xfail_message ]]; then
			log_test_xfail "$grp-$counter on $int" "$xfail_message"
			continue;
		fi
		check_err "$err" "$grp-$counter is not valid on $int (expected $target, got $delta)"
		log_test "$grp-$counter on $int"
	done
}

test_eth_ctrl_stats()
{
	local pkt_format="-a own -b bcast 88:08 -p 64"
	local num_pkts=1000
	local -a counters

	counters=("$h1 eth-ctrl MACControlFramesTransmitted $num_pkts 0")
	traffic_test "$h1" "$h2" "$num_pkts" "$pkt_format" \
		"${counters[@]}"

	counters=("$h1 eth-ctrl MACControlFramesReceived $num_pkts 0")
	traffic_test "$h2" "$h1" "$num_pkts" "$pkt_format" \
		"${counters[@]}"
}

test_eth_mac_stats()
{
	local pkt_size=100
	local pkt_size_fcs=$((pkt_size + 4))
	local bcast_pkt_format="-a own -b bcast -p $pkt_size"
	local mcast_pkt_format="-a own -b 01:00:5E:00:00:01 -p $pkt_size"
	local ucast_pkt_format="-a own -b $h2_mac -p $pkt_size"
	local num_pkts=2000
	local octets=$((pkt_size_fcs * num_pkts))
	local -a counters error_cnt collision_cnt

	# Error counters should be exactly zero
	counters=("$h1 eth-mac FrameCheckSequenceErrors 0 1"
		  "$h1 eth-mac AlignmentErrors 0 1"
		  "$h1 eth-mac FramesLostDueToIntMACXmitError 0 1"
		  "$h1 eth-mac CarrierSenseErrors 0 1"
		  "$h1 eth-mac FramesLostDueToIntMACRcvError 0 1"
		  "$h1 eth-mac InRangeLengthErrors 0 1"
		  "$h1 eth-mac OutOfRangeLengthField 0 1"
		  "$h1 eth-mac FrameTooLongErrors 0 1"
		  "$h1 eth-mac FramesAbortedDueToXSColls 0 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$bcast_pkt_format" \
		"${counters[@]}"

	# Collision related counters should also be zero
	counters=("$h1 eth-mac SingleCollisionFrames 0 1"
		  "$h1 eth-mac MultipleCollisionFrames 0 1"
		  "$h1 eth-mac FramesWithDeferredXmissions 0 1"
		  "$h1 eth-mac LateCollisions 0 1"
		  "$h1 eth-mac FramesWithExcessiveDeferral 0 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$bcast_pkt_format" \
		"${counters[@]}"

	counters=("$h1 eth-mac BroadcastFramesXmittedOK $num_pkts 0"
		  "$h1 eth-mac OctetsTransmittedOK $octets 0")
	traffic_test "$h1" "$h2" "$num_pkts" "$bcast_pkt_format" \
		"${counters[@]}"

	counters=("$h1 eth-mac BroadcastFramesReceivedOK $num_pkts 0"
		  "$h1 eth-mac OctetsReceivedOK $octets 0")
	traffic_test "$h2" "$h1" "$num_pkts" "$bcast_pkt_format" \
		"${counters[@]}"

	counters=("$h1 eth-mac FramesTransmittedOK $num_pkts 0"
		  "$h1 eth-mac MulticastFramesXmittedOK $num_pkts 0")
	traffic_test "$h1" "$h2" "$num_pkts" "$mcast_pkt_format" \
		"${counters[@]}"

	counters=("$h1 eth-mac FramesReceivedOK $num_pkts 0"
		  "$h1 eth-mac MulticastFramesReceivedOK $num_pkts 0")
	traffic_test "$h2" "$h1" "$num_pkts" "$mcast_pkt_format" \
		"${counters[@]}"
}

test_pause_stats()
{
	local pkt_format="-a own -b 01:80:c2:00:00:01 88:08:00:01:00:01"
	local xfail_message="software sent pause frames not detected"
	local num_pkts=2000
	local -a counters
	local int
	local i

	# Check that there is pause frame support
	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		int="${NETIFS[p$i]}"
		if ! run_on "$int" ethtool -I --json -a "$int" > /dev/null 2>&1; then
			log_test_skip "No support for pause frames, skip tests"
			exit
		fi
	done

	counters=("$h1 pause tx_pause_frames $num_pkts 0 $xfail_message")
	traffic_test "$h1" "$h2" "$num_pkts" "$pkt_format" \
		"${counters[@]}"

	counters=("$h1 pause rx_pause_frames $num_pkts 0")
	traffic_test "$h2" "$h1" "$num_pkts" "$pkt_format" \
		"${counters[@]}"
}

setup_prepare()
{
	local iface

	h1=${NETIFS[p1]}
	h2=${NETIFS[p2]}

	h2_mac=$(mac_get "$h2")
}

check_ethtool_counter_group_support
trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit "$EXIT_STATUS"
