#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2020 Intel Corporation.

ksft_pass=0
ksft_fail=1
ksft_xfail=2
ksft_xpass=3
ksft_skip=4

XSKOBJ=lib/xskxceiver

validate_root_exec()
{
	msg="skip all tests:"
	if [ $UID != 0 ]; then
		echo $msg must be run as root >&2
		test_exit $ksft_fail
	else
		return $ksft_pass
	fi
}

validate_veth_support()
{
	msg="skip all tests:"
	if [ $(ip link add $1 type veth 2>/dev/null; echo $?;) != 0 ]; then
		echo $msg veth kernel support not available >&2
		test_exit $ksft_skip
	else
		ip link del $1
		return $ksft_pass
	fi
}

test_status()
{
	statusval=$1
	if [ $statusval -eq $ksft_fail ]; then
		echo "$2: [ FAIL ]"
	elif [ $statusval -eq $ksft_skip ]; then
		echo "$2: [ SKIPPED ]"
	elif [ $statusval -eq $ksft_pass ]; then
		echo "$2: [ PASS ]"
	fi
}

test_exit()
{
	if [ $1 -ne 0 ]; then
		test_status $1 $(basename $0)
	fi
	exit 1
}

clear_configs()
{
	[ $(ip link show $1 &>/dev/null; echo $?;) == 0 ] &&
		{ ip link del $1; }
}

cleanup_exit()
{
	clear_configs $1 $2
}

validate_ip_utility()
{
	[ ! $(type -P ip) ] && { echo "'ip' not found. Skipping tests."; test_exit $ksft_skip; }
}

wait_port_listen()
{
	local i

	for i in $(seq 50); do
		if [ -n "$(ss -Hltn "sport = :$1" 2>/dev/null)" ]; then
			return 0
		fi
		kill -0 $2 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

random_port()
{
	local i port

	for i in $(seq 50); do
		port=$((32768 + RANDOM))
		if [ -z "$(ss -Hltn "sport = :$port" 2>/dev/null)" ]; then
			echo "$port"
			return 0
		fi
	done
	return 1
}

# The TX endpoint listens on the control port and runs in the background,
# the RX endpoint connects to it and reports; both print the same verdicts.
exec_xskxceiver()
{
	local tx_log tx_pid tx_ret run_args="${ARGS}"

        if [[ $busy_poll -eq 1 ]]; then
		run_args+=" -b"
	fi

	tx_log=$(mktemp)
	./${XSKOBJ} -i ${VETH0} -e tx -p ${PEER_HOST} -P ${PEER_PORT} ${run_args} > ${tx_log} 2>&1 &
	tx_pid=$!

	if wait_port_listen ${PEER_PORT} ${tx_pid}; then
		./${XSKOBJ} -i ${VETH1} -e rx -p ${PEER_HOST} -P ${PEER_PORT} ${run_args}
		retval=$?
	else
		echo "TX endpoint did not listen on ${PEER_HOST}:${PEER_PORT}"
		retval=1
	fi

	wait ${tx_pid}
	tx_ret=$?
	if [[ $tx_ret -ne 0 || $verbose -eq 1 ]]; then
		sed 's/^/tx| /' ${tx_log}
	fi
	rm -f ${tx_log}
	[[ $retval -eq 0 ]] && retval=$tx_ret

	if [[ $list -ne 1 ]]; then
	    test_status $retval "${TEST_NAME}"
	    statusList+=($retval)
	    nameList+=(${TEST_NAME})
	fi
}
