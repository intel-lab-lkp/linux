#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
#

DIR="$(dirname $(readlink -f "$0"))"

KTAP_HELPERS="${DIR}/../kselftest/ktap_helpers.sh"
if [ -e "$KTAP_HELPERS" ]; then
    . "$KTAP_HELPERS"
else
    echo -n "1..0 # SKIP $KTAP_HELPERS file not found"
    exit 4
fi

RPROC_SYS=/sys/class/remoteproc
RPROC_SEQ_SLEEP=5

rproc_instances=
num_tests=0
test_err=0

check_error() {
	if [ $? -ne 0 ]; then
		test_err=$((test_err+1))
		ktap_print_msg "$@"
	fi
}

parse_args() {
	script=${0##*/}

	if [ $# -eq 2 ] && [ "$1" = "--seqdelay" ]; then
		shift || true
		RPROC_SEQ_SLEEP=$1
	else
		ktap_print_msg "Usage: ${script} --seqdelay <time in secs>"
		ktap_print_msg "Proceed with default sequence delay = $RPROC_SEQ_SLEEP"
	fi
}

rproc_stop_instances() {
	for instance in ${rproc_instances}; do
		rproc=${RPROC_SYS}/$instance
		rproc_name=$(cat $rproc/name)
		rproc_state=$(cat $rproc/state)

		echo stop > "$rproc/state"
		check_error "$rproc_name state-stop failed at state $rproc_state"
	done
	sleep ${RPROC_SEQ_SLEEP}
}

rproc_start_instances() {
	for instance in ${rproc_instances}; do
		rproc=${RPROC_SYS}/$instance
		rproc_name=$(cat $rproc/name)
		rproc_state=$(cat $rproc/state)

		echo start > "$rproc/state"
		check_error "$rproc_name state-start failed at state $rproc_state"
	done
	sleep ${RPROC_SEQ_SLEEP}
}

rproc_seq_test_instance_one() {
	instance=$1
	rproc=${RPROC_SYS}/$instance
	rproc_name=$(cat $rproc/name)
	rproc_state=$(cat $rproc/state)
	ktap_print_msg "Testing rproc sequence for $rproc_name"

	# Reset test_err value
	test_err=0

	# Begin start/stop sequence
	echo start > "$rproc/state"
	check_error "$rproc_name state-start failed at state $rproc_state"

	sleep ${RPROC_SEQ_SLEEP}

	echo stop > "$rproc/state"
	check_error "$rproc_name state-stop failed at state $rproc_state"

	if [ $test_err -ne 0 ]; then
		ktap_test_fail "$rproc_name"
	else
		ktap_test_pass "$rproc_name"
	fi
}

rproc_seq_test_instances_concurrently() {
	# Reset test_err value
	test_err=0

	rproc_start_instances

	rproc_stop_instances

	if [ $test_err -ne 0 ]; then
		ktap_test_fail "for any of $rproc_instances"
	else
		ktap_test_pass "for all $rproc_instances"
	fi
}

#################################
### Test starts here
#################################

ktap_print_header

# Parse user arguments
parse_args $@

# Check for required sysfs entries
if [ ! -d "${RPROC_SYS}" ]; then
	ktap_skip_all "${RPROC_SYS} doesn't exist."
	exit "${KSFT_SKIP}"
fi

rproc_instances=$(find ${RPROC_SYS}/remoteproc* -maxdepth 1 -exec basename {} \;)
num_tests=$(echo ${rproc_instances} | wc -w)
if [ "${num_tests}" -eq 0 ]; then
	ktap_skip_all "${RPROC_SYS}/remoteproc* doesn't exist."
	exit "${KSFT_SKIP}"
fi

# Total tests will be:
# 1) Seq tests for each instance sequencially
# 2) Seq tests for all instances concurrently
num_tests=$((num_tests+1))

ktap_set_plan "${num_tests}"

### Stop all instances
#
# Intention is to stop all running instances. If any instances are not yet
# started it will be don't care case as test_err is not checked.
# NOTE: Assuming no instances are in crashed state
rproc_stop_instances

### Test 1
ktap_print_msg "Testing rproc start/stop sequence for each instance sequencially"
for instance in ${rproc_instances}; do
	rproc_seq_test_instance_one $instance
done

### Test 2
ktap_print_msg "Testing rproc start/stop sequence for all instances concurrently"
rproc_seq_test_instances_concurrently

### Restore all instances
rproc_start_instances

ktap_finished
