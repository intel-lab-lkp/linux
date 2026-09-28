#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="
	test_isotp_frame_type
"

net_dir=$(dirname $0)/..
source $net_dir/lib.sh

export CANIF=${CANIF:-"vcan0"}

# set only when setup() creates the interface
created=

setup()
{
	if [[ $CANIF != vcan* ]]; then
		echo "test_isotp_frame_type requires a vcan device" >&2
		exit $ksft_skip
	fi

	# CANIF defaults to vcan0, which may already exist and be in use; only
	# take it down again if this run is the one that created it.
	if ip link show dev $CANIF >/dev/null 2>&1; then
		echo "$CANIF already exists, leaving it in place" >&2
	else
		ip link add name $CANIF type vcan || exit $ksft_skip
		created=1
	fi

	# a vcan device defaults to CANXL_MTU, so it carries every frame
	# type this test needs without further configuration
	ip link set dev $CANIF mtu 2060 2>/dev/null
	ip link set dev $CANIF up
}

cleanup()
{
	[[ -n $created ]] || return
	ip link set dev $CANIF down
	ip link delete $CANIF
}

test_isotp_frame_type()
{
	./test_isotp_frame_type
	check_err $?
	log_test "test_isotp_frame_type"
}

trap cleanup EXIT
setup

tests_run

exit $EXIT_STATUS
