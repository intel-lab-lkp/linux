#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="
	test_cts_hold
"

net_dir=$(dirname $0)/..
source $net_dir/lib.sh

export CANIF=${CANIF:-"vcan0"}
BITRATE=${BITRATE:-500000}

setup()
{
	if [[ $CANIF == vcan* ]]; then
		ip link add name $CANIF type vcan || exit $ksft_skip
	else
		ip link set dev $CANIF type can bitrate $BITRATE || exit $ksft_skip
	fi
	ip link set dev $CANIF up
	pwd
}

cleanup()
{
	ip link set dev $CANIF down
	if [[ $CANIF == vcan* ]]; then
		ip link delete $CANIF
	fi
}

test_cts_hold()
{
	./test_cts_hold
	check_err $?
	log_test "test_cts_hold"
}

trap cleanup EXIT
setup

tests_run

exit $EXIT_STATUS
