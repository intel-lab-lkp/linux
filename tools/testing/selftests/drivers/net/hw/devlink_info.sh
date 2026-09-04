#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test devlink info support
#
# This test logs the device information a driver reports through the devlink
# info interface, in the format NIPA CI consumes for regression tracking (see
# the Device information section at
# https://github.com/linux-netdev/nipa/wiki/Netdev-CI-system).
#
# Implementing devlink info is optional, so a device whose driver reports no
# versions and no serial number is skipped rather than failed.
#
# Usage:
#   NETIF=eth0 ./devlink_info.sh

ALL_TESTS="devlink_info_test"

lib_dir=$(dirname "$0")

# NETIF may also be provided through drivers/net/net.config, as documented in
# drivers/net/README.rst. Source it before lib.sh so that a stray assignment
# cannot clobber the framework's globals.
if [[ -z "$NETIF" && -f "$lib_dir/../net.config" ]]; then
	source "$lib_dir/../net.config"
fi

source "$lib_dir"/../../../net/lib.sh

require_command devlink
require_command jq

DL_HANDLE=
DL_INFO=

setup_prepare()
{
	local err

	if [[ -z "$NETIF" ]]; then
		log_test_skip "devlink info" "NETIF is not configured"
		exit "$EXIT_STATUS"
	fi

	# Try to get the devlink handle from the devlink port first.
	DL_HANDLE=$(devlink -j port show 2>/dev/null |
		jq -r --arg netif "$NETIF" \
			'.port | to_entries[] |
			 select(.value.netdev == $netif) | .key' 2>/dev/null |
		head -n 1 |
		cut -d/ -f1-2)

	# Fall back to the PCI address reported by ethtool. Devices on other
	# buses are only found through the devlink port lookup above.
	if [[ -z "$DL_HANDLE" ]] && command -v ethtool >/dev/null; then
		local bus_info

		bus_info=$(ethtool -i "$NETIF" 2>/dev/null |
			awk '/^bus-info:/ {print $2}')
		if [[ -n "$bus_info" ]] &&
		   devlink dev show "pci/$bus_info" &>/dev/null; then
			DL_HANDLE="pci/$bus_info"
		fi
	fi

	if [[ -z "$DL_HANDLE" ]]; then
		log_test_skip "devlink info" "no devlink handle for $NETIF"
		exit "$EXIT_STATUS"
	fi

	# Query once so that a single snapshot is validated throughout.
	DL_INFO=$(devlink -j dev info "$DL_HANDLE" 2>/dev/null)
	err=$?
	if ((err)); then
		log_test_skip "devlink info" "devlink dev info failed for $DL_HANDLE"
		exit "$EXIT_STATUS"
	fi
}

# jq's "// empty" maps a missing or null field to no output, so callers get an
# empty string rather than the literal text "null".
info_get()
{
	local name=$1

	jq -r --arg name "$name" '.[][][$name] // empty' <<<"$DL_INFO"
}

log_versions()
{
	local versions line

	versions=$(jq -r '.[][].versions // {} | to_entries[] | .key as $type |
			  .value | to_entries[] |
			  "\(.key) (\($type)): \(.value)"' <<<"$DL_INFO" \
			  2>/dev/null)

	while IFS= read -r line; do
		[[ -n "$line" ]] && log_info "$line"
	done <<<"$versions"
}

has_any_version()
{
	jq -e '.[][].versions // {} | [.[] | to_entries[]] | length > 0' \
		<<<"$DL_INFO" &>/dev/null
}

devlink_info_test()
{
	RET=0

	local driver serial board_serial

	driver=$(info_get "driver")
	serial=$(info_get "serial_number")
	board_serial=$(info_get "board.serial_number")

	# devlink reports the driver name for every registered instance, even
	# when the driver does not implement info_get. Everything else is
	# optional, so a device with nothing further to report is not a
	# failure.
	if ! has_any_version && [[ -z "$serial" && -z "$board_serial" ]]; then
		log_test_skip "devlink info" "no info reported for $DL_HANDLE"
		return
	fi

	# Whether the driver name appears in the JSON depends on the installed
	# iproute2, so report it when present but do not require it.
	[[ -n "$driver" ]] && log_info "driver: $driver"

	# devlink names the driver bound to the parent device, which can
	# legitimately differ from the netdev's ethtool driver, so report a
	# difference without failing.
	if command -v ethtool >/dev/null; then
		local ethtool_driver

		ethtool_driver=$(ethtool -i "$NETIF" 2>/dev/null |
			awk '/^driver:/ {print $2}')
		if [[ -n "$driver" && -n "$ethtool_driver" &&
		      "$driver" != "$ethtool_driver" ]]; then
			log_info "driver mismatch: devlink='$driver' ethtool='$ethtool_driver'"
		fi
	fi

	[[ -n "$serial" ]] && log_info "serial_number: $serial"
	[[ -n "$board_serial" ]] && log_info "board.serial_number: $board_serial"

	log_versions

	log_test "devlink info"
}

setup_prepare

tests_run

exit "$EXIT_STATUS"
