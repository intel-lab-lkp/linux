#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026 Google LLC
# Author: Priyank Rathod <rathodpriyank@google.com>
#
# Kselftest for PCIe Lane Margining at Receiver (LMR / LMT)
# Tests the debugfs interface exposed by drivers/pci/pcie/margin.c
# (/sys/kernel/debug/pci/pcie_lmr_<pci_dev_name>/)

set -e

TESTNAME="pcie_lmt"

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
retval=0
skipmsg="skip all tests:"

if [ $UID != 0 ]; then
	echo "$skipmsg must be run as root" >&2
	exit $ksft_skip
fi

DEBUGFS=$(mount -t debugfs | head -1 | awk '{ print $3 }')
if [ -z "$DEBUGFS" ]; then
	if [ -d "/sys/kernel/debug" ]; then
		DEBUGFS="/sys/kernel/debug"
	else
		echo "$skipmsg debugfs is not mounted" >&2
		exit $ksft_skip
	fi
fi

if [ ! -d "$DEBUGFS/pci" ]; then
	# Allow searching debugfs root or pci directory
	:
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

echo "$TESTNAME: testing PCIe LMR debugfs entries"

for dev in $LMR_DEVS; do
	dev_name=$(basename "$dev")
	echo "$TESTNAME: probing device $dev_name"

	if [ ! -r "$dev/capabilities" ] || [ ! -r "$dev/port_status" ] ||
	   [ ! -r "$dev/enable" ] || [ ! -w "$dev/enable" ]; then
		echo "$TESTNAME: $dev_name missing mandatory root attributes"
		retval=1
		continue
	fi

	caps=$(cat "$dev/capabilities")
	status=$(cat "$dev/port_status")
	echo "  $dev_name: capabilities read OK"
	echo "  $dev_name: port_status read OK"

	trap 'cleanup_dev "$dev"' EXIT

	if ! echo 1 > "$dev/enable" 2>/dev/null; then
		echo "  $dev_name: margining not ready by hardware (skipping active lanes)"
		continue
	fi

	echo "  $dev_name: margining enabled OK"

	for lane_dir in $(ls -d "$dev"/lane* 2>/dev/null || true); do
		lane=$(basename "$lane_dir")
		echo "  $dev_name: testing $lane"

		# Test setting receiver (Rx 0 is always local receiver)
		echo 0 > "$lane_dir/receiver"
		cat "$lane_dir/caps" > /dev/null
		cat "$lane_dir/num_timing_steps" > /dev/null
		cat "$lane_dir/num_voltage_steps" > /dev/null

		# Test resetting timing and voltage margin
		echo 0 > "$lane_dir/margin_timing"
		echo 0 > "$lane_dir/margin_voltage"
	done

	echo 0 > "$dev/enable"
	trap - EXIT
	echo "  $dev_name: margining disabled OK"
done

if [ $retval -eq 0 ]; then
	echo "$TESTNAME [PASS]"
else
	echo "$TESTNAME [FAIL]"
fi

exit $retval
