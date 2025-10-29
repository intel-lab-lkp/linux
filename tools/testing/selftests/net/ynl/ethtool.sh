#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Test YNL ethtool functionality

# shellcheck disable=SC1091,SC2034,SC2154,SC2317
source ../lib.sh

readonly NSIM_ID="1337"
readonly NSIM_DEV_NAME="nsim${NSIM_ID}"
readonly VETH_A="veth_a"
readonly VETH_B="veth_b"

ALL_TESTS="
	ethtool_device_info
	ethtool_statistics
	ethtool_ring_params
	ethtool_coalesce_params
	ethtool_pause_params
	ethtool_features_info
	ethtool_channels_info
	ethtool_time_stamping
"

# Uses veth device as netdevsim doesn't support basic ethtool device info
ethtool_device_info()
{
	RET=0

	local info_output
	info_output=$(ip netns exec "$testns" ynl-ethtool "$VETH_A" 2>/dev/null)
	check_err $? "failed to get device info for $VETH_A"

	echo "$info_output" | grep -q "Settings for"
	check_err $? "device info output missing expected content"

	log_test "YNL ethtool device info"
}

ethtool_statistics()
{
	RET=0

	local stats_output
	stats_output=$(ip netns exec "$testns" ynl-ethtool --statistics "$NSIM_DEV_NAME" 2>/dev/null)
	check_err $? "failed to get statistics for $NSIM_DEV_NAME"

	echo "$stats_output" | grep -q -E "(NIC statistics|packets|bytes)"
	check_err $? "statistics output missing expected content"

	log_test "YNL ethtool statistics"
}

ethtool_ring_params()
{
	RET=0

	local ring_output
	ring_output=$(ip netns exec "$testns" ynl-ethtool --show-ring "$NSIM_DEV_NAME" 2>/dev/null)
	check_err $? "failed to get ring parameters for $NSIM_DEV_NAME"

	if [[ -n "$ring_output" ]]; then
		echo "$ring_output" | grep -q -E "(Ring parameters|RX|TX)"
		check_err $? "ring parameters output missing expected content"
	fi

	if ! ip netns exec "$testns" ynl-ethtool --set-ring "$NSIM_DEV_NAME" rx 64 2>/dev/null; then
		check_err 1 "set-ring command failed unexpectedly"
	fi

	log_test "YNL ethtool ring parameters (show/set)"
}

ethtool_coalesce_params()
{
	RET=0

	ip netns exec "$testns" ynl-ethtool --show-coalesce "$NSIM_DEV_NAME" &>/dev/null
	check_err $? "failed to get coalesce parameters for $NSIM_DEV_NAME"

	if ! ip netns exec "$testns" ynl-ethtool --set-coalesce "$NSIM_DEV_NAME" rx-usecs 50 2>/dev/null; then
		check_err 1 "set-coalesce command failed unexpectedly"
	fi

	log_test "YNL ethtool coalesce parameters (show/set)"
}

ethtool_pause_params()
{
	RET=0

	ip netns exec "$testns" ynl-ethtool --show-pause "$NSIM_DEV_NAME" &>/dev/null
	check_err $? "failed to get pause parameters for $NSIM_DEV_NAME"

	if ! ip netns exec "$testns" ynl-ethtool --set-pause "$NSIM_DEV_NAME" tx 1 rx 1 2>/dev/null; then
		check_err 1 "set-pause command failed unexpectedly"
	fi

	log_test "YNL ethtool pause parameters (show/set)"
}

ethtool_features_info()
{
	RET=0

	local features_output
	features_output=$(ip netns exec "$testns" ynl-ethtool --show-features "$NSIM_DEV_NAME" 2>/dev/null)
	check_err $? "failed to get features for $NSIM_DEV_NAME"

	if [[ -n "$features_output" ]]; then
		echo "$features_output" | grep -q -E "(Features|offload)"
		check_err $? "features output missing expected content"
	fi

	log_test "YNL ethtool features info (show/set)"
}

ethtool_channels_info()
{
	RET=0

	local channels_output
	channels_output=$(ip netns exec "$testns" ynl-ethtool --show-channels "$NSIM_DEV_NAME" 2>/dev/null)
	check_err $? "failed to get channels for $NSIM_DEV_NAME"

	if [[ -n "$channels_output" ]]; then
		echo "$channels_output" | grep -q -E "(Channel|Combined|RX|TX)"
		check_err $? "channels output missing expected content"
	fi

	if ! ip netns exec "$testns" ynl-ethtool --set-channels "$NSIM_DEV_NAME" combined-count 1 2>/dev/null; then
		check_err 1 "set-channels command failed unexpectedly"
	fi

	log_test "YNL ethtool channels info (show/set)"
}

ethtool_time_stamping()
{
	RET=0

	local ts_output
	ts_output=$(ip netns exec "$testns" ynl-ethtool --show-time-stamping "$NSIM_DEV_NAME" 2>/dev/null)
	check_err $? "failed to get time stamping info for $NSIM_DEV_NAME"

	if [[ -n "$ts_output" ]]; then
		echo "$ts_output" | grep -q -E "(Time stamping|timestamping|SOF_TIMESTAMPING)"
		check_err $? "time stamping output missing expected content"
	fi

	log_test "YNL ethtool time stamping"
}

setup()
{
	if ! modprobe netdevsim &>/dev/null; then
		log_test_skip "all YNL ethtool tests" "netdevsim module not available"
		exit "$ksft_skip"
	fi

	setup_ns testns

	if ! create_netdevsim "$NSIM_ID" "$testns" >/dev/null 2>&1; then
		log_test_skip "all YNL ethtool tests" "failed to create netdevsim device"
		exit "$ksft_skip"
	fi

	if ! ip -n "$testns" link add "$VETH_A" type veth peer name "$VETH_B" 2>/dev/null; then
		log_test_skip "all YNL ethtool tests" "failed to create veth pair"
		exit "$ksft_skip"
	fi

	ip -n "$testns" link set "$VETH_A" up
	ip -n "$testns" link set "$VETH_B" up
}

cleanup()
{
	cleanup_netdevsim "$NSIM_ID" 2>/dev/null
	cleanup_all_ns
}

trap cleanup EXIT

require_command "ynl-ethtool"
setup
tests_run

exit "$EXIT_STATUS"
