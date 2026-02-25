#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#shellcheck disable=SC2034 # SC does not see the global variables

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

traffic_test()
{
	local iface=$1; shift
	local neigh=$1; shift
	local num_tx=$1; shift
	local pkt_format="$1"; shift
	local title="$1"; shift
	declare -a counters=( "${@:2:$1}" ); shift "$(( $1 + 1 ))"
	local before after delta target_high extra
	local int grp counter target unit
	local num_rx=$((num_tx * 2))
	local xfail_message
	local src="aggregate"

	for i in "${!counters[@]}"; do
		read -r int grp counter target unit xfail_message <<< "${counters[$i]}"
		before[i]=$(ethtool_std_stats_get "$int" "$grp" "$counter" "$src")
	done

	# shellcheck disable=SC2086 # needs split options
	$MZ "$iface" -q -c "$num_tx" $pkt_format

	# shellcheck disable=SC2086 # needs split options
	$MZ "$neigh" -q -c "$num_rx" $pkt_format

	for i in "${!counters[@]}"; do
		read -r int grp counter target unit xfail_message<<< "${counters[$i]}"

		after[i]=$(ethtool_std_stats_get "$int" "$grp" "$counter" "$src")
		if [[ "${after[$i]}" == "null" ]]; then
			log_test_skip "$int does not support $grp-$counter"
			continue;
		fi

		delta=$((after[i] - before[i]))

		# Allow an extra 1% tolerance for random packets sent by the stack
		extra=$((num_pkts * unit / 100))
		target_high=$((target + extra))

		RET=0
		[ "$delta" -ge "$target" ] && [ "$delta" -le "$target_high" ]
		err="$?"
		if [[ $err != 0  ]] && [[ -n $xfail_message ]]; then
			log_test_xfail "$xfail_message"
			continue;
		fi
		check_err "$err" "$grp-$counter is not valid on $int (expected $target, got $delta)"
		log_test "$title" "$counter on $int"
	done
}

test_eth_ctrl_stats()
{
	local pkt_format="-a own -b bcast 88:08 -p 64"
	local num_pkts=1000
	local counters

	counters=("$h1 eth-ctrl MACControlFramesTransmitted $num_pkts 1"
		  "$h2 eth-ctrl MACControlFramesReceived $num_pkts 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$pkt_format" \
		"eth-ctrl tx on $h1" \
		"${#counters[@]}" "${counters[@]}"

	counters=("$h2 eth-ctrl MACControlFramesTransmitted $num_pkts 1"
		  "$h1 eth-ctrl MACControlFramesReceived $num_pkts 1")
	traffic_test "$h2" "$h1" "$num_pkts" "$pkt_format" \
		"eth-ctrl tx on $h2" \
		"${#counters[@]}" "${counters[@]}"
}

test_eth_mac_stats()
{
	local pkt_size=100
	local pkt_size_fcs=$((pkt_size + 4))
	local bcast_pkt_format="-a own -b bcast -p $pkt_size"
	local mcast_pkt_format="-a own -b -b 01:00:5E:00:00:01 -p $pkt_size"
	local ucast_pkt_format="-a own -b $h2_mac -p $pkt_size"
	local num_pkts=2000
	local octets=$((pkt_size_fcs * num_pkts))
	local counters

	counters=("$h1 eth-mac BroadcastFramesXmittedOK $num_pkts 1"
		  "$h1 eth-mac FramesTransmittedOK $num_pkts 1"
		  "$h1 eth-mac OctetsTransmittedOK $octets $pkt_size_fcs"
		  "$h1 eth-mac MulticastFramesXmittedOK 0 1"
		  "$h2 eth-mac BroadcastFramesReceivedOK $num_pkts 1"
		  "$h2 eth-mac FramesReceivedOK $num_pkts 1"
		  "$h2 eth-mac OctetsReceivedOK $octets $pkt_size_fcs"
		  "$h2 eth-mac MulticastFramesReceivedOK 0 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$bcast_pkt_format" \
		"eth-mac bcast tx on $h1" \
		"${#counters[@]}" "${counters[@]}"

	counters=("$h1 eth-mac BroadcastFramesXmittedOK 0 1"
		  "$h1 eth-mac FramesTransmittedOK $num_pkts 1"
		  "$h1 eth-mac OctetsTransmittedOK $octets $pkt_size_fcs"
		  "$h1 eth-mac MulticastFramesXmittedOK $num_pkts 1"
		  "$h2 eth-mac BroadcastFramesReceivedOK 0 1"
		  "$h2 eth-mac FramesReceivedOK $num_pkts 1"
		  "$h2 eth-mac OctetsReceivedOK $octets $pkt_size_fcs"
		  "$h2 eth-mac MulticastFramesReceivedOK $num_pkts 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$mcast_pkt_format" \
		"eth-mac mcast tx on $h1" \
		"${#counters[@]}" "${counters[@]}"

	counters=("$h1 eth-mac BroadcastFramesXmittedOK 0 1"
		  "$h1 eth-mac FramesTransmittedOK $num_pkts 1"
		  "$h1 eth-mac OctetsTransmittedOK $octets $pkt_size_fcs"
		  "$h1 eth-mac MulticastFramesXmittedOK 0 1"
		  "$h2 eth-mac BroadcastFramesReceivedOK 0 1"
		  "$h2 eth-mac FramesReceivedOK $num_pkts 1"
		  "$h2 eth-mac OctetsReceivedOK $octets $pkt_size_fcs"
		  "$h2 eth-mac MulticastFramesReceivedOK 0 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$ucast_pkt_format" \
		"eth-mac ucast tx on $h1" \
		"${#counters[@]}" "${counters[@]}"
}

test_pause_stats()
{
	local pkt_format="-a own -b 01:80:c2:00:00:01 88:08:00:01:00:01"
	local xfail_message="Not all MACs detect injected pause frames"
	local num_pkts=2000
	local counters i

	# Check that there is pause frame support
	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		if ! ethtool -I --json -a "${NETIFS[p$i]}" > /dev/null 2>&1; then
			log_test_skip "No support for pause frames, skip tests"
			exit
		fi
	done

	counters=("$h1 pause tx_pause_frames $num_pkts 1 $xfail_message"
		  "$h2 pause rx_pause_frames $num_pkts 1")
	traffic_test "$h1" "$h2" "$num_pkts" "$pkt_format" \
		"pause tx on $h1" \
		"${#counters[@]}" "${counters[@]}"

	counters=("$h2 pause tx_pause_frames $num_pkts 1 $xfail_message"
		  "$h1 pause rx_pause_frames $num_pkts 1")
	traffic_test "$h2" "$h1" "$num_pkts" "$pkt_format" \
		"pause tx on $h2" \
		"${#counters[@]}" "${counters[@]}"
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	h2=${NETIFS[p2]}

	h2_mac=$(mac_get "$h2")

	for iface in $h1 $h2; do
		ip link set dev "$iface" up
	done
}

cleanup()
{
	pre_cleanup

	for iface in $h2 $h1; do
		ip link set dev "$iface" down
	done
}

check_ethtool_counter_group_support
trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit "$EXIT_STATUS"
