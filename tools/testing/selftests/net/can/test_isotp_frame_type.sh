#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="
	test_isotp_frame_type
"

net_dir=$(dirname $0)/..
source $net_dir/lib.sh

export CANIF=${CANIF:-"vcan0"}

setup()
{
	if [[ $CANIF == vcan* ]]; then
		ip link add name $CANIF type vcan || exit $ksft_skip
		# a vcan device defaults to CANXL_MTU, so it carries every frame
		# type this test needs without further configuration
		ip link set dev $CANIF mtu 2060 2>/dev/null
	else
		echo "test_isotp_frame_type requires a vcan device" >&2
		exit $ksft_skip
	fi
	ip link set dev $CANIF up
}

cleanup()
{
	ip link set dev $CANIF down
	if [[ $CANIF == vcan* ]]; then
		ip link delete $CANIF
	fi
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
