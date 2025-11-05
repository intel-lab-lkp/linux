#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Test YNL ethtool functionality

# Default ynl-ethtool path for direct execution, can be overridden by make install
ynl_ethtool="../pyynl/ethtool.py"

readonly NSIM_ID="1337"
readonly NSIM_DEV_NAME="nsim${NSIM_ID}"
readonly VETH_A="veth_a"
readonly VETH_B="veth_b"

testns="ynl-ethtool-$(mktemp -u XXXXXX)"

# Uses veth device as netdevsim doesn't support basic ethtool device info
ethtool_device_info() {
	local info_output

	info_output=$(ip netns exec "$testns" $ynl_ethtool "$VETH_A" 2>/dev/null)

	if ! echo "$info_output" | grep -q "Settings for"; then
		echo "FAIL: YNL ethtool device info (device info output missing expected content)"
		return
	fi

	echo "PASS: YNL ethtool device info"
}

ethtool_statistics() {
	local stats_output

	stats_output=$(ip netns exec "$testns" $ynl_ethtool --statistics "$NSIM_DEV_NAME" 2>/dev/null)

	if ! echo "$stats_output" | grep -q -E "(NIC statistics|packets|bytes)"; then
		echo "FAIL: YNL ethtool statistics (statistics output missing expected content)"
		return
	fi

	echo "PASS: YNL ethtool statistics"
}

ethtool_ring_params() {
	local ring_output

	ring_output=$(ip netns exec "$testns" $ynl_ethtool --show-ring "$NSIM_DEV_NAME" 2>/dev/null)

	if ! echo "$ring_output" | grep -q -E "(Ring parameters|RX|TX)"; then
		echo "FAIL: YNL ethtool ring parameters (ring parameters output missing expected content)"
		return
	fi

	if ! ip netns exec "$testns" $ynl_ethtool --set-ring "$NSIM_DEV_NAME" rx 64 2>/dev/null; then
		echo "FAIL: YNL ethtool ring parameters (set-ring command failed unexpectedly)"
		return
	fi

	echo "PASS: YNL ethtool ring parameters (show/set)"
}

ethtool_coalesce_params() {
	if ! ip netns exec "$testns" $ynl_ethtool --show-coalesce "$NSIM_DEV_NAME" &>/dev/null; then
		echo "FAIL: YNL ethtool coalesce parameters (failed to get coalesce parameters)"
		return
	fi

	if ! ip netns exec "$testns" $ynl_ethtool --set-coalesce "$NSIM_DEV_NAME" rx-usecs 50 2>/dev/null; then
		echo "FAIL: YNL ethtool coalesce parameters (set-coalesce command failed unexpectedly)"
		return
	fi

	echo "PASS: YNL ethtool coalesce parameters (show/set)"
}

ethtool_pause_params() {
	if ! ip netns exec "$testns" $ynl_ethtool --show-pause "$NSIM_DEV_NAME" &>/dev/null; then
		echo "FAIL: YNL ethtool pause parameters (failed to get pause parameters)"
		return
	fi

	if ! ip netns exec "$testns" $ynl_ethtool --set-pause "$NSIM_DEV_NAME" tx 1 rx 1 2>/dev/null; then
		echo "FAIL: YNL ethtool pause parameters (set-pause command failed unexpectedly)"
		return
	fi

	echo "PASS: YNL ethtool pause parameters (show/set)"
}

ethtool_features_info() {
	local features_output

	features_output=$(ip netns exec "$testns" $ynl_ethtool --show-features "$NSIM_DEV_NAME" 2>/dev/null)

	if ! echo "$features_output" | grep -q -E "(Features|offload)"; then
		echo "FAIL: YNL ethtool features info (features output missing expected content)"
		return
	fi

	echo "PASS: YNL ethtool features info (show/set)"
}

ethtool_channels_info() {
	local channels_output

	channels_output=$(ip netns exec "$testns" $ynl_ethtool --show-channels "$NSIM_DEV_NAME" 2>/dev/null)

	if ! echo "$channels_output" | grep -q -E "(Channel|Combined|RX|TX)"; then
		echo "FAIL: YNL ethtool channels info (channels output missing expected content)"
		return
	fi

	if ! ip netns exec "$testns" $ynl_ethtool --set-channels "$NSIM_DEV_NAME" combined-count 1 2>/dev/null; then
		echo "FAIL: YNL ethtool channels info (set-channels command failed unexpectedly)"
		return
	fi

	echo "PASS: YNL ethtool channels info (show/set)"
}

ethtool_time_stamping() {
	local ts_output

	ts_output=$(ip netns exec "$testns" $ynl_ethtool --show-time-stamping "$NSIM_DEV_NAME" 2>/dev/null)

	if ! echo "$ts_output" | grep -q -E "(Time stamping|timestamping|SOF_TIMESTAMPING)"; then
		echo "FAIL: YNL ethtool time stamping (time stamping output missing expected content)"
		return
	fi

	echo "PASS: YNL ethtool time stamping"
}

setup() {
	if ! modprobe netdevsim &>/dev/null; then
		echo "SKIP: all YNL ethtool tests (netdevsim module not available)"
		exit 0
	fi

	if ! ip netns add "$testns" 2>/dev/null; then
		echo "SKIP: all YNL ethtool tests (failed to create test namespace)"
		exit 0
	fi

	echo "$NSIM_ID 1" | ip netns exec "$testns" tee /sys/bus/netdevsim/new_device >/dev/null 2>&1 || {
		echo "SKIP: all YNL ethtool tests (failed to create netdevsim device)"
		exit 0
	}

	local dev
	dev=$(ip netns exec "$testns" ls /sys/bus/netdevsim/devices/netdevsim$NSIM_ID/net 2>/dev/null | head -1)
	if [[ -z "$dev" ]]; then
		echo "SKIP: all YNL ethtool tests (failed to find netdevsim device)"
		exit 0
	fi

	ip -netns "$testns" link set dev "$dev" name "$NSIM_DEV_NAME" 2>/dev/null || {
		echo "SKIP: all YNL ethtool tests (failed to rename netdevsim device)"
		exit 0
	}

	ip -netns "$testns" link set dev "$NSIM_DEV_NAME" up 2>/dev/null

	if ! ip -n "$testns" link add "$VETH_A" type veth peer name "$VETH_B" 2>/dev/null; then
		echo "SKIP: all YNL ethtool tests (failed to create veth pair)"
		exit 0
	fi

	ip -n "$testns" link set "$VETH_A" up 2>/dev/null
	ip -n "$testns" link set "$VETH_B" up 2>/dev/null
}

cleanup() {
	if [[ -n "$testns" ]]; then
		ip netns exec "$testns" bash -c "echo $NSIM_ID > /sys/bus/netdevsim/del_device" 2>/dev/null || true
		ip netns del "$testns" 2>/dev/null || true
	fi
}

# Check if ynl-ethtool command is available
if ! command -v $ynl_ethtool &>/dev/null && [[ ! -x $ynl_ethtool ]]; then
	echo "SKIP: all YNL ethtool tests (ynl-ethtool command not found: $ynl_ethtool)"
	exit 0
fi

trap cleanup EXIT
setup

# Run all tests
ethtool_device_info
ethtool_statistics
ethtool_ring_params
ethtool_coalesce_params
ethtool_pause_params
ethtool_features_info
ethtool_channels_info
ethtool_time_stamping
