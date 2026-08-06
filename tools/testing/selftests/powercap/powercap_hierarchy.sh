#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_pass=0
ksft_fail=1
ksft_skip=4

MODULE=powercap_hierarchy
CONTROL=powercap-test
SYSFS=/sys/devices/virtual/powercap/$CONTROL

fail()
{
	echo "FAIL: $*"
	exit $ksft_fail
}

skip()
{
	echo "SKIP: $*"
	exit $ksft_skip
}

cleanup()
{
	if lsmod | grep -q "^${MODULE}\b"; then
		if ! rmmod "$MODULE"; then
			echo "WARNING: failed to unload $MODULE"
		fi
	fi
}

trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || skip "must be run as root"

insmod ./powercap_hierarchy.ko || fail "failed to load module"

[ -d "$SYSFS" ] || fail "missing $SYSFS"

check_zone()
{
	zone=$1

	[ -f "$zone/name" ] || fail "$zone/name missing"

	[ -f "$zone/power_uw" ] || \
		fail "$zone/power_uw missing"

	[ -f "$zone/max_power_range_uw" ] || \
		fail "$zone/max_power_range_uw missing"

	power=$(cat "$zone/power_uw")
	[ "$power" = "12648430" ] || \
		fail "$zone: unexpected power_uw ($power)"

	max=$(cat "$zone/max_power_range_uw")
	[ "$max" = "50159747054" ] || \
		fail "$zone: unexpected max_power_range_uw ($max)"

	constraint="$zone/constraint_0"

	name=$(cat "$constraint""_name")
	[ "$name" = "my constraint name" ] || \
		fail "$constraint: bad constraint name"

	pl=$(cat "$constraint""_power_limit_uw")
	[ "$pl" = "3735929054" ] || \
		fail "$constraint: bad power limit"

	mp=$(cat "$constraint""_max_power_uw")
	[ "$mp" = "3735929054" ] || \
		fail "$constraint: bad max power"
}

zones=0

find "$SYSFS" -type f -name name | while read namefile
do
	zone=$(dirname "$namefile")

	case "$zone" in
		*/constraint_*)
			continue
			;;
	esac

	check_zone "$zone"

	zones=$((zones + 1))
done

#
# Count the number of powercap zones.
#
count=$(find "$SYSFS" -type f -name name | \
	grep -v constraint | wc -l)

[ "$count" -eq 16 ] || \
	fail "expected 16 zones, got $count"

#
# Explicitly unload the module.
#
cleanup

#
# Verify that the hierarchy disappeared.
#
if [ -d "$SYSFS" ]; then
	fail "$SYSFS still exists after module removal"
fi

trap - EXIT INT TERM

echo "PASS"

exit $ksft_pass
