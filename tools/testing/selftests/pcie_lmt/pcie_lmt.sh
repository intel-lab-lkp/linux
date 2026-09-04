#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026 Google LLC
# Author: Priyank Rathod <rathodpriyank@google.com>
#
# Kselftest for PCIe Lane Margining at Receiver (LMR / LMT)
# Tests the debugfs interface exposed by drivers/pci/pcie/margin.c
# (/sys/kernel/debug/pci/pcie_lmr_<pci_dev_name>/)

TESTNAME="pcie_lmt"

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
retval=0
skipmsg="skip all tests:"

# Rejection test: must be run as root
if [ "$UID" -ne 0 ]; then
	echo "$skipmsg must be run as root" >&2
	exit $ksft_skip
fi

SCRIPT_PATH=$(realpath "$0" 2>/dev/null || echo "$0")

# Test non-root user execution rejection via subshell if unprivileged user exists
test_non_root_rejection()
{
	local non_root_cmd=""
	local exit_code=0

	if command -v runuser >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
		non_root_cmd="runuser -u nobody --"
	elif command -v su >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
		non_root_cmd="su -s /bin/bash nobody -c"
	fi

	if [ -n "$non_root_cmd" ]; then
		$non_root_cmd "$SCRIPT_PATH" >/dev/null 2>&1 || exit_code=$?
		if [ "$exit_code" -eq "$ksft_skip" ]; then
			echo "$TESTNAME: non-root user execution rejection test [PASS]"
		else
			echo "$TESTNAME: non-root run got $exit_code (exp $ksft_skip) [FAIL]" >&2
			retval=1
		fi
	else
		echo "$TESTNAME: skipping non-root subshell test (no unprivileged user/su)"
	fi
}

test_non_root_rejection

DEBUGFS=$(mount -t debugfs | head -1 | awk '{ print $3 }')
if [ -z "$DEBUGFS" ]; then
	if [ -d "/sys/kernel/debug" ]; then
		DEBUGFS="/sys/kernel/debug"
	else
		echo "$skipmsg debugfs is not mounted" >&2
		exit $ksft_skip
	fi
fi

LMR_DEVS=$(ls -d $DEBUGFS/pci/pcie_lmr_* $DEBUGFS/pcie_lmr_* 2>/dev/null || true)
if [ -z "$LMR_DEVS" ]; then
	echo "$skipmsg no PCIe LMR devices found in $DEBUGFS/" >&2
	exit $ksft_skip
fi

cleanup_dev()
{
	local dev="$1"
	echo 0 > "$dev/enable" 2>/dev/null || true
}

assert_write_fail()
{
	local file="$1"
	local val="$2"
	local desc="$3"
	local fname

	fname=$(basename "$file")
	if (echo "$val" > "$file") 2>/dev/null; then
		echo "    FAIL: $desc ('$val' -> $fname succeeded, expected fail)" >&2
		retval=1
	else
		echo "    PASS: $desc rejected correctly"
	fi
}

assert_write_success()
{
	local file="$1"
	local val="$2"
	local desc="$3"
	local fname

	fname=$(basename "$file")
	if ! (echo "$val" > "$file") 2>/dev/null; then
		echo "    FAIL: $desc ('$val' -> $fname failed, expected success)" >&2
		retval=1
	else
		echo "    PASS: $desc succeeded correctly"
	fi
}

assert_read_fail()
{
	local file="$1"
	local desc="$2"

	if cat "$file" >/dev/null 2>&1; then
		echo "    FAIL: $desc (reading $(basename "$file") succeeded, expected failure)" >&2
		retval=1
	else
		echo "    PASS: $desc rejected correctly"
	fi
}

assert_read_success()
{
	local file="$1"
	local desc="$2"

	if ! cat "$file" >/dev/null 2>&1; then
		echo "    FAIL: $desc (reading $(basename "$file") failed, expected success)" >&2
		retval=1
	else
		echo "    PASS: $desc succeeded correctly"
	fi
}

echo "$TESTNAME: testing PCIe LMR debugfs entries"

for dev in $LMR_DEVS; do
	dev_name=$(basename "$dev")
	echo "$TESTNAME: probing device $dev_name"

	if [ ! -r "$dev/capabilities" ] || [ ! -r "$dev/port_status" ] ||
	   [ ! -r "$dev/enable" ] || [ ! -w "$dev/enable" ]; then
		echo "$TESTNAME: $dev_name missing mandatory root attributes" >&2
		retval=1
		continue
	fi

	assert_read_success "$dev/capabilities" "$dev_name: capabilities read"
	assert_read_success "$dev/port_status" "$dev_name: port_status read"

	# Negative test: write to read-only root files
	echo "  $dev_name: testing read-only root attributes"
	assert_write_fail "$dev/capabilities" "0" "write to read-only capabilities"
	assert_write_fail "$dev/port_status" "0" "write to read-only port_status"

	# Negative test: operations while margining is disabled
	echo "  $dev_name: testing operations while disabled"
	for lane_dir in $(ls -d "$dev"/lane* 2>/dev/null || true); do
		lane=$(basename "$lane_dir")
		assert_write_fail "$lane_dir/margin_timing" "1" \
			"$lane: timing step while disabled"
		assert_write_fail "$lane_dir/margin_voltage" "1" \
			"$lane: voltage step while disabled"
		assert_read_fail "$lane_dir/caps" \
			"$lane: read caps while disabled"
		assert_read_fail "$lane_dir/num_timing_steps" \
			"$lane: read timing steps while disabled"
		assert_read_fail "$lane_dir/num_voltage_steps" \
			"$lane: read voltage steps while disabled"
		break
	done

	# Negative test: invalid enable inputs
	echo "  $dev_name: testing invalid enable inputs"
	assert_write_fail "$dev/enable" "invalid" "enable invalid string"
	assert_write_fail "$dev/enable" "2" "enable invalid numeric '2'"
	assert_write_fail "$dev/enable" "-1" "enable negative numeric '-1'"
	assert_write_fail "$dev/enable" "999" "enable out-of-bounds '999'"
	assert_write_fail "$dev/enable" "" "enable empty string"

	trap 'cleanup_dev "$dev"' EXIT INT TERM

	if ! echo 1 > "$dev/enable" 2>/dev/null; then
		echo "  $dev_name: margining not ready by hardware (skipping active lanes)"
		trap - EXIT INT TERM
		continue
	fi

	echo "  $dev_name: margining enabled OK"

	for lane_dir in $(ls -d "$dev"/lane* 2>/dev/null || true); do
		lane=$(basename "$lane_dir")
		echo "  $dev_name: testing $lane (active)"

		# Negative test: write to read-only lane attributes
		assert_write_fail "$lane_dir/caps" "0" \
			"$lane: write to read-only caps"
		assert_write_fail "$lane_dir/num_timing_steps" "0" \
			"$lane: write to read-only num_timing_steps"
		assert_write_fail "$lane_dir/num_voltage_steps" "0" \
			"$lane: write to read-only num_voltage_steps"

		# Negative test: invalid receiver numbers (valid: 0..6, 7 reserved)
		assert_write_fail "$lane_dir/receiver" "7" "$lane: receiver 7 (reserved)"
		assert_write_fail "$lane_dir/receiver" "8" "$lane: receiver 8 (> 6)"
		assert_write_fail "$lane_dir/receiver" "255" "$lane: receiver 255"
		assert_write_fail "$lane_dir/receiver" "-1" "$lane: negative receiver -1"
		assert_write_fail "$lane_dir/receiver" "invalid" "$lane: non-numeric receiver"

		# Set valid receiver 0 (local receiver)
		assert_write_success "$lane_dir/receiver" "0" "$lane: set receiver 0 (local)"

		# Read capabilities and step limits
		assert_read_success "$lane_dir/caps" "$lane: read caps"
		assert_read_success "$lane_dir/num_timing_steps" "$lane: read num_timing_steps"
		assert_read_success "$lane_dir/num_voltage_steps" "$lane: read num_voltage_steps"

		caps_raw=$(cat "$lane_dir/caps" 2>/dev/null || true)
		num_timing=$(cat "$lane_dir/num_timing_steps" 2>/dev/null || echo 0)
		num_voltage=$(cat "$lane_dir/num_voltage_steps" 2>/dev/null || echo 0)

		# Negative test: out-of-bounds timing steps (spec limit: 0..63)
		assert_write_fail "$lane_dir/margin_timing" "9999" \
			"$lane: timing step 9999 (out of range)"
		assert_write_fail "$lane_dir/margin_timing" "-9999" \
			"$lane: timing step -9999 (out of range)"
		assert_write_fail "$lane_dir/margin_timing" "64" \
			"$lane: timing step 64 (> spec max 63)"
		assert_write_fail "$lane_dir/margin_timing" "-64" \
			"$lane: timing step -64 (< spec min -63)"
		assert_write_fail "$lane_dir/margin_timing" "invalid" \
			"$lane: non-numeric timing step"

		if [ -n "$num_timing" ] && [ "$num_timing" -ge 0 ] 2>/dev/null; then
			assert_write_fail "$lane_dir/margin_timing" "$((num_timing + 1))" \
				"$lane: timing step > receiver limit ($num_timing)"
		fi

		# Negative test: negative timing on symmetric receiver
		if echo "$caps_raw" | grep -q "Left/Right: symmetric"; then
			assert_write_fail "$lane_dir/margin_timing" "-1" \
				"$lane: negative timing on symmetric receiver"
		fi

		# Negative test: out-of-bounds voltage steps (spec limit: 0..127)
		assert_write_fail "$lane_dir/margin_voltage" "9999" \
			"$lane: voltage step 9999 (out of range)"
		assert_write_fail "$lane_dir/margin_voltage" "-9999" \
			"$lane: voltage step -9999 (out of range)"
		assert_write_fail "$lane_dir/margin_voltage" "128" \
			"$lane: voltage step 128 (> spec max 127)"
		assert_write_fail "$lane_dir/margin_voltage" "-128" \
			"$lane: voltage step -128 (< spec min -127)"
		assert_write_fail "$lane_dir/margin_voltage" "invalid" \
			"$lane: non-numeric voltage step"

		if echo "$caps_raw" | grep -q "Voltage Supported: No"; then
			assert_write_fail "$lane_dir/margin_voltage" "1" \
				"$lane: voltage step on unsupported receiver"
		else
			if [ -n "$num_voltage" ] && [ "$num_voltage" -ge 0 ] 2>/dev/null; then
				assert_write_fail "$lane_dir/margin_voltage" \
					"$((num_voltage + 1))" \
					"$lane: voltage step > receiver limit ($num_voltage)"
			fi
			if echo "$caps_raw" | grep -q "Up/Down: symmetric"; then
				assert_write_fail "$lane_dir/margin_voltage" "-1" \
					"$lane: negative voltage on symmetric receiver"
			fi
		fi

		# Positive test: reset timing and voltage margin to 0 (nominal settings)
		assert_write_success "$lane_dir/margin_timing" "0" "$lane: reset timing to 0"
		assert_write_success "$lane_dir/margin_voltage" "0" "$lane: reset voltage to 0"
	done

	# Cleanly disable margining
	echo 0 > "$dev/enable"
	trap - EXIT INT TERM
	echo "  $dev_name: margining disabled OK"
done

if [ $retval -eq 0 ]; then
	echo "$TESTNAME [PASS]"
else
	echo "$TESTNAME [FAIL]"
fi

exit $retval
