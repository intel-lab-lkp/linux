#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

EINJ_TABLE=/sys/firmware/acpi/tables/EINJ
EINJ_DEBUGFS=/sys/kernel/debug/apei/einj
NVIDIA_PLATFORM_GLOB=/sys/bus/platform/devices/NVDA2012:*
NVIDIA_DRIVER_DIR=/sys/bus/platform/drivers/nvidia-ghes

einj_skip()
{
	echo "$0: $1" >&2
	exit $ksft_skip
}

einj_require_root()
{
	[ "$(id -u)" -eq 0 ] || einj_skip "must be run as root"
}

einj_require_debugfs()
{
	[ -d /sys/kernel/debug ] || einj_skip "debugfs is not mounted at /sys/kernel/debug"
}

einj_require_einj()
{
	[ -e "$EINJ_TABLE" ] || einj_skip "ACPI EINJ table is missing"
	if [ ! -d "$EINJ_DEBUGFS" ]; then
		modprobe einj 2>/dev/null || true
	fi
	[ -d "$EINJ_DEBUGFS" ] || einj_skip "EINJ debugfs directory is missing"
}

einj_require_vendor_einj()
{
	[ -e "$EINJ_DEBUGFS/vendor" ] || einj_skip "NVIDIA vendor EINJ metadata is missing"
	[ -e "$EINJ_DEBUGFS/vendor_flags" ] || einj_skip "NVIDIA vendor EINJ flags are missing"
}

einj_require_available_error_type()
{
	local available

	available=$(einj_read_trimmed_value available_error_type)
	[ -n "$available" ] || einj_skip "available_error_type is missing"
}

einj_read_trimmed_value()
{
	local file=$1

	einj_read_value "$file" | tr -d '\n'
}

einj_require_writable_value()
{
	local file=$1

	[ -w "$EINJ_DEBUGFS/$file" ] || einj_skip "$file is not writable"
}

einj_require_writable_profile()
{
	local file

	for file in error_type flags vendor_flags param1 param2 param3 param4 notrigger; do
		einj_require_writable_value "$file"
	done
}

einj_find_bound_nvidia_device()
{
	local dev

	for dev in $NVIDIA_PLATFORM_GLOB; do
		[ -e "$dev" ] || continue
		if [ "$(readlink -f "$dev/driver" 2>/dev/null)" = "$NVIDIA_DRIVER_DIR" ]; then
			echo "$dev"
			return 0
		fi
	done

	return 1
}

einj_require_bound_nvidia_device()
{
	local dev

	dev=$(einj_find_bound_nvidia_device) || einj_skip "no bound NVIDIA GHES device"
	echo "$dev"
}

einj_read_value()
{
	local file=$1

	cat "$EINJ_DEBUGFS/$file"
}

einj_write_value()
{
	local file=$1
	local value=$2

	printf '%s\n' "$value" > "$EINJ_DEBUGFS/$file"
}

einj_restore_value()
{
	local file=$1
	local value=$2

	# Some EINJ controls read back as an empty string when unset, but the
	# debugfs write handler has no matching "clear" operation.
	[ -n "$value" ] || return 0
	einj_write_value "$file" "$value"
}

einj_save_state()
{
	EINJ_SAVED_ERROR_TYPE=$(einj_read_value error_type)
	EINJ_SAVED_FLAGS=$(einj_read_value flags)
	EINJ_SAVED_PARAM1=$(einj_read_value param1)
	EINJ_SAVED_PARAM2=$(einj_read_value param2)
	EINJ_SAVED_PARAM3=$(einj_read_value param3)
	EINJ_SAVED_PARAM4=$(einj_read_value param4)
	EINJ_SAVED_VENDOR_FLAGS=$(einj_read_value vendor_flags)
	EINJ_SAVED_NOTRIGGER=$(einj_read_value notrigger)
}

einj_restore_state()
{
	[ -n "${EINJ_SAVED_ERROR_TYPE+x}" ] || return 0

	einj_restore_value error_type "$EINJ_SAVED_ERROR_TYPE"
	einj_restore_value flags "$EINJ_SAVED_FLAGS"
	einj_restore_value param1 "$EINJ_SAVED_PARAM1"
	einj_restore_value param2 "$EINJ_SAVED_PARAM2"
	einj_restore_value param3 "$EINJ_SAVED_PARAM3"
	einj_restore_value param4 "$EINJ_SAVED_PARAM4"
	einj_restore_value vendor_flags "$EINJ_SAVED_VENDOR_FLAGS"
	einj_restore_value notrigger "$EINJ_SAVED_NOTRIGGER"
}

einj_emit_kmsg_marker()
{
	local tag=$1
	local marker

	marker="ghes-nvidia-einj:${tag}:$$:${RANDOM}"
	printf '%s\n' "$marker" > /dev/kmsg
	printf '%s\n' "$marker"
}

einj_capture_dmesg_after_marker()
{
	local marker=$1

	dmesg | awk -v marker="$marker" '
		found { print }
		index($0, marker) { found = 1 }
	'
}

einj_wait_for_dmesg_after_marker_contains()
{
	local marker=$1
	local needle=$2
	local timeout=${3:-10}
	local i
	local slice

	for i in $(seq 1 "$timeout"); do
		slice=$(einj_capture_dmesg_after_marker "$marker")
		if printf '%s\n' "$slice" | grep -Fq "$needle"; then
			printf '%s\n' "$slice"
			return 0
		fi
		sleep 1
	done

	return 1
}
