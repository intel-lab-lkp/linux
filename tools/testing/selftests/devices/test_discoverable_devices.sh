#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2023 Collabora Ltd
#
# This script tests for presence and driver binding of devices from discoverable
# busses (ie USB, PCI) on Devicetree-based platforms.
#
# The per-platform list of devices to be tested is stored inside the boards/
# directory and chosen based on compatible.
#

DIR="$(dirname "$(readlink -f "$0")")"

source "${DIR}"/ktap_helpers.sh

KSFT_FAIL=1
KSFT_SKIP=4

retval=0

usb()
{
	name="$1"
	controller="$2"
	path="$3"
	configuration="$4"
	interfaces="$5"

	# Extract additional match if present
	if [[ "$controller" =~ , ]]; then
		additional_match=${controller#*,}
		address=${controller%,*}
	else
		address="$controller"
	fi

	for controller_uevent in /sys/bus/usb/devices/usb*/uevent; do
		if grep -q "OF_FULLNAME=.*@$address$" "$controller_uevent"; then
			# Look for additional match if present. It is needed to
			# disambiguate two USB busses that share the same
			# controller.
			if [ -n "$additional_match" ]; then
				if ! grep -q "$additional_match" "$controller_uevent"; then
					continue
				fi
			fi
			dir=$(basename "$(dirname "$controller_uevent")")
			busnum=${dir#usb}
		fi
	done

	usbdevs=/sys/bus/usb/devices/

	IFS=,
	for intf in $interfaces; do
		devfile="$busnum"-"$path":"$configuration"."$intf"

		if [ -d "$usbdevs"/"$devfile" ]; then
			ktap_test_pass usb."$name"."$intf".device
		else
			ktap_test_fail usb."$name"."$intf".device
			retval=$KSFT_FAIL
		fi

		if [ -d "$usbdevs"/"$devfile"/driver ]; then
			ktap_test_pass usb."$name"."$intf".driver
		else
			ktap_test_fail usb."$name"."$intf".driver
			retval=$KSFT_FAIL
		fi
	done
}

pci()
{
	name="$1"
	controller="$2"
	path="$3"

	IFS=$'\n'
	while read -r uevent; do
		grep -q "OF_FULLNAME=.*@$controller$" "$uevent" || continue

		# Ignore PCI bus directory, since it will have the same backing
		# OF node, but not the PCI devices as subdirectories.
		[[ "$uevent" =~ pci_bus ]] && continue

		host_dir=$(dirname "$uevent")
	done < <(find /sys/devices -name uevent)

	# Add * to each level of the PCI hierarchy so we can rely on globbing to
	# find the device directory on sysfs.
	globbed_path=$(echo "$path" | sed -e 's|^|*|' -e 's|/|/*|')
	device_path="$host_dir/pci*/$globbed_path"

	# Intentionally left unquoted to allow the glob to expand
	if [ -d $device_path ]; then
		ktap_test_pass pci."$name".device
	else
		ktap_test_fail pci."$name".device
		retval=$KSFT_FAIL
	fi

	if [ -d $device_path/driver ]; then
		ktap_test_pass pci."$name".driver
	else
		ktap_test_fail pci."$name".driver
		retval=$KSFT_FAIL
	fi
}

count_tests()
{
	board_file="$1"
	num_tests=0

	# Each USB interface in a single USB test in the board file is a
	# separate test
	while read -r line; do
		num_intfs=$(echo "$line" | tr -dc , | wc -c)
		num_intfs=$((num_intfs + 1))
		num_tests=$((num_tests + num_intfs))
	done < <(grep ^usb "$board_file" | cut -d ' ' -f 6 -)

	num_pci=$(grep -c ^pci "$board_file")
	num_tests=$((num_tests + num_pci))

	# Account for device and driver test for each of the tests listed in the
	# board file.
	num_tests=$((num_tests * 2))
	echo $num_tests
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

ktap_set_plan "$(count_tests "$board_file")"

source "$board_file"

ktap_print_totals
exit "${retval}"
