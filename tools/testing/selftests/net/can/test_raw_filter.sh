#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

#set -x

ALL_TESTS="
	test_raw_filter
"

net_dir=$(dirname $0)/..
source $net_dir/lib.sh

VCANIF="vcan0"

setup()
{
	ip link add name $VCANIF type vcan || exit $ksft_skip
	ip link set dev $VCANIF up
	pwd
}

cleanup()
{
	ip link delete $VCANIF
}

test_raw_filter()
{
	./test_raw_filter
}

trap cleanup EXIT
setup

tests_run

exit $EXIT_STATUS
