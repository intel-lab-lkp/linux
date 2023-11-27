#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2023 Collabora Ltd
#
# This script tests for presence and driver binding of devices from discoverable
# busses (ie USB, PCI).
#
# The per-platform list of devices to be tested is stored inside the boards/
# directory and chosen based on compatible. Each line of the file follows the
# following format:
#
#   usb|pci test_name number_of_matches field=value [ field=value ... ]
#
# The available match fields vary by bus. The field-value match pairs for a
# device can be retrieved from the device's modalias attribute in sysfs.
#

DIR="$(dirname $(readlink -f "$0"))"

source "${DIR}"/ktap_helpers.sh

KSFT_FAIL=1
KSFT_SKIP=4

retval=0

match()
{
	FILE="$1"
	ID="$2"

	[ ! -f "$FILE" ] && return 1
	[ "$ID" = "" ] && return 0
	grep -q "$ID" "$FILE" || return 1
	return 0
}

usb()
{
	name="$1"
	count="$2"
	shift 2

	for arg in $@; do
		[[ "$arg" =~ ^v= ]] && v="${arg#v=}"
		[[ "$arg" =~ ^p= ]] && p="${arg#p=}"
		[[ "$arg" =~ ^d= ]] && d="${arg#d=}"
		[[ "$arg" =~ ^dc= ]] && dc="${arg#dc=}"
		[[ "$arg" =~ ^dsc= ]] && dsc="${arg#dsc=}"
		[[ "$arg" =~ ^dp= ]] && dp="${arg#dp=}"
		[[ "$arg" =~ ^ic= ]] && ic="${arg#ic=}"
		[[ "$arg" =~ ^isc= ]] && isc="${arg#isc=}"
		[[ "$arg" =~ ^ip= ]] && ip="${arg#ip=}"
		[[ "$arg" =~ ^in= ]] && in="${arg#in=}"
	done

	cur_count=0

	for dev in $(find /sys/bus/usb/devices -maxdepth 1); do
		match "$dev"/idVendor "$v" || continue
		match "$dev"/idProduct "$p" || continue
		match "$dev"/bcdDevice "$d" || continue
		match "$dev"/bDeviceClass "$dc" || continue
		match "$dev"/bDeviceSubClass "$dsc" || continue
		match "$dev"/bDeviceProtocol "$dp" || continue

		# Matched device. Now search through interfaces
		for intf in $(find "$dev"/ -maxdepth 1 -type d); do
			match "$intf"/bInterfaceClass "$ic" || continue
			match "$intf"/bInterfaceSubClass "$isc" || continue
			match "$intf"/bInterfaceProtocol "$ip" || continue
			match "$intf"/bInterfaceNumber "$in" || continue

			# Matched interface. Add to count if it was probed by driver.
			[ -d "$intf"/driver ] && cur_count=$((cur_count+1))
		done
	done

	if [ "$cur_count" -eq "$count" ]; then
		ktap_test_pass usb."$name"
	else
		ktap_test_fail usb."$name"
		retval="$KSFT_FAIL"
	fi
}

pci()
{
	name="$1"
	count="$2"
	shift 2

	for arg in $@; do
		[[ "$arg" =~ ^v= ]] && v="${arg#v=}"
		[[ "$arg" =~ ^d= ]] && d="${arg#d=}"
		[[ "$arg" =~ ^sv= ]] && sv="${arg#sv=}"
		[[ "$arg" =~ ^sd= ]] && sd="${arg#sd=}"
		[[ "$arg" =~ ^bc= ]] && bc="${arg#bc=}"
		[[ "$arg" =~ ^sc= ]] && sc="${arg#sc=}"
		[[ "$arg" =~ ^i= ]] && i="${arg#i=}"
	done

	cur_count=0

	for dev in $(find /sys/bus/pci/devices -maxdepth 1); do
		match "$dev"/vendor "$v" || continue
		match "$dev"/device "$d" || continue
		match "$dev"/subsystem_vendor "$sv" || continue
		match "$dev"/subsystem_device "$sd" || continue

		[ -z "$bc" ] && bc='..'
		[ -z "$sc" ] && sc='..'
		[ -z "$i" ] && i='..'
		match "$dev/"class "$bc$sc$i" || continue

		# Matched device. Add to count if it was probed by driver.
		[ -d "$dev"/driver ] && cur_count=$((cur_count+1))
	done

	if [ "$cur_count" -eq "$count" ]; then
		ktap_test_pass pci."$name"
	else
		ktap_test_fail pci."$name"
		retval="$KSFT_FAIL"
	fi
}

ktap_print_header

plat_compatible=/proc/device-tree/compatible

if [ ! -f "$plat_compatible" ]; then
	ktap_skip_all "No board compatible available"
	exit "$KSFT_SKIP"
fi

compatibles=$(tr '\000' '\n' < "$plat_compatible")

for compatible in $compatibles; do
	if [ -f boards/"$compatible" ]; then
		board_file=boards/"$compatible"
		break
	fi
done

if [ -z "$board_file" ]; then
	ktap_skip_all "No matching board file found"
	exit "$KSFT_SKIP"
fi

echo "# Using board file: " "$board_file"

num_tests=$(grep -E "^(usb|pci)" "$board_file" | wc -l)
ktap_set_plan "$num_tests"

source "$board_file"

ktap_print_totals
exit "${retval}"
