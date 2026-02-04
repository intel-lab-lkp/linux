#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# See virtio_net_common.sh comments for more details about assumed setup

ALL_TESTS="
	initial_ping_test
	f_mac_test
	buffer_circulation_test
"

source virtio_net_common.sh

lib_dir=$(dirname "$0")
source "$lib_dir"/../../../net/forwarding/lib.sh

h1=${NETIFS[p1]}
h2=${NETIFS[p2]}

IPERF_SERVER_PID=""

h1_create()
{
	simple_if_init $h1 $H1_IPV4/24 $H1_IPV6/64
}

h1_destroy()
{
	simple_if_fini $h1 $H1_IPV4/24 $H1_IPV6/64
}

h2_create()
{
	simple_if_init $h2 $H2_IPV4/24 $H2_IPV6/64
}

h2_destroy()
{
	simple_if_fini $h2 $H2_IPV4/24 $H2_IPV6/64
}

initial_ping_test()
{
	setup_cleanup
	setup_prepare
	ping_test $h1 $H2_IPV4 " simple"
}

f_mac_test()
{
	RET=0
	local test_name="mac feature filtered"

	virtio_feature_present $h1 $VIRTIO_NET_F_MAC
	if [ $? -ne 0 ]; then
		log_test_skip "$test_name" "Device $h1 is missing feature $VIRTIO_NET_F_MAC."
		return 0
	fi
	virtio_feature_present $h1 $VIRTIO_NET_F_MAC
	if [ $? -ne 0 ]; then
		log_test_skip "$test_name" "Device $h2 is missing feature $VIRTIO_NET_F_MAC."
		return 0
	fi

	setup_cleanup
	setup_prepare

	grep -q 0 /sys/class/net/$h1/addr_assign_type
	check_err $? "Permanent address assign type for $h1 is not set"
	grep -q 0 /sys/class/net/$h2/addr_assign_type
	check_err $? "Permanent address assign type for $h2 is not set"

	setup_cleanup
	virtio_filter_feature_add $h1 $VIRTIO_NET_F_MAC
	virtio_filter_feature_add $h2 $VIRTIO_NET_F_MAC
	setup_prepare

	grep -q 0 /sys/class/net/$h1/addr_assign_type
	check_fail $? "Permanent address assign type for $h1 is set when F_MAC feature is filtered"
	grep -q 0 /sys/class/net/$h2/addr_assign_type
	check_fail $? "Permanent address assign type for $h2 is set when F_MAC feature is filtered"

	ping_do $h1 $H2_IPV4
	check_err $? "Ping failed"

	log_test "$test_name"
}

buffer_circulation_test()
{
	RET=0
	local test_name="buffer circulation"
	local tracefs="/sys/kernel/tracing"

	if ! check_command iperf3; then
		log_test_skip "$test_name" "iperf3 not installed"
		return 0
	fi

	setup_cleanup
	setup_prepare

	ping -c 1 -I "$h1" "$H2_IPV4" >/dev/null
	if [ $? -ne 0 ]; then
		check_err 1 "Ping failed"
		log_test "$test_name"
		return
	fi

	local rx_start=$(cat /sys/class/net/"$h2"/statistics/rx_packets)
	local tx_start=$(cat /sys/class/net/"$h1"/statistics/tx_packets)

	if [ -d "$tracefs/events/page_pool" ]; then
		echo > "$tracefs/trace"
		echo 1 > "$tracefs/events/page_pool/enable"
	fi

	local port=$(shuf -i 49152-65535 -n 1)

	iperf3 -s -1 --bind-dev "$h2" -p "$port" &>/dev/null &
	IPERF_SERVER_PID=$!
	sleep 1

	if ! kill -0 "$IPERF_SERVER_PID" 2>/dev/null; then
		IPERF_SERVER_PID=""
		if [ -d "$tracefs/events/page_pool" ]; then
			echo 0 > "$tracefs/events/page_pool/enable"
		fi
		check_err 1 "iperf3 server died"
		log_test "$test_name"
		return
	fi

	iperf3 -c "$H2_IPV4" --bind-dev "$h1" -p "$port" -t 5 >/dev/null 2>&1
	local iperf_ret=$?

	if [ -n "$IPERF_SERVER_PID" ]; then
		kill "$IPERF_SERVER_PID" 2>/dev/null || true
		wait "$IPERF_SERVER_PID" 2>/dev/null || true
		IPERF_SERVER_PID=""
	fi

	if [ -d "$tracefs/events/page_pool" ]; then
		echo 0 > "$tracefs/events/page_pool/enable"
		local trace="$tracefs/trace"
		local hold=$(grep -c "page_pool_state_hold" "$trace" 2>/dev/null)
		local release=$(grep -c "page_pool_state_release" "$trace" 2>/dev/null)
		log_info "page_pool events: hold=${hold:-0}, release=${release:-0}"
	fi

	local rx_end=$(cat /sys/class/net/"$h2"/statistics/rx_packets)
	local tx_end=$(cat /sys/class/net/"$h1"/statistics/tx_packets)
	local rx_delta=$((rx_end - rx_start))
	local tx_delta=$((tx_end - tx_start))

	log_info "Circulated TX:$tx_delta RX:$rx_delta"

	if [ "$iperf_ret" -ne 0 ]; then
		check_err 1 "iperf3 failed"
	elif [ "$rx_delta" -lt 10000 ]; then
		check_err 1 "Too few packets: $rx_delta"
	fi

	log_test "$test_name"
}

setup_prepare()
{
	virtio_device_rebind $h1
	virtio_device_rebind $h2
	wait_for_dev $h1
	wait_for_dev $h2

	vrf_prepare

	h1_create
	h2_create
}

setup_cleanup()
{
	h2_destroy
	h1_destroy

	vrf_cleanup

	virtio_filter_features_clear $h1
	virtio_filter_features_clear $h2
	virtio_device_rebind $h1
	virtio_device_rebind $h2
	wait_for_dev $h1
	wait_for_dev $h2
}

cleanup()
{
	if [ -n "$IPERF_SERVER_PID" ]; then
		kill "$IPERF_SERVER_PID" 2>/dev/null || true
		wait "$IPERF_SERVER_PID" 2>/dev/null || true
	fi

	pre_cleanup
	setup_cleanup
}

check_driver $h1 "virtio_net"
check_driver $h2 "virtio_net"
check_virtio_debugfs $h1
check_virtio_debugfs $h2

trap cleanup EXIT

setup_prepare

tests_run

exit "$EXIT_STATUS"
