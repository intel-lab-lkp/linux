#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This test is for checking IPv6 ICMP behavior in different situations.
source lib.sh
ret=0

# all tests in this script, can be overridden with -t option
TESTS="icmpv6_to_local_address"

VERBOSE=0
PAUSE_ON_FAIL=no
PAUSE=no

which ping6 > /dev/null 2>&1 && ping6=$(which ping6) || ping6=$(which ping)

log_test()
{
	local rc=$1
	local expected=$2
	local msg="$3"

	if [ ${rc} -eq ${expected} ]; then
		printf "    TEST: %-60s  [OK]\n" "${msg}"
		nsuccess=$((nsuccess+1))
	else
		ret=1
		nfail=$((nfail+1))
		printf "    TEST: %-60s  [FAIL]\n" "${msg}"
		if [ "${PAUSE_ON_FAIL}" = "yes" ]; then
		echo
			echo "hit enter to continue, 'q' to quit"
			read a
			[ "$a" = "q" ] && exit 1
		fi
	fi

	if [ "${PAUSE}" = "yes" ]; then
		echo
		echo "hit enter to continue, 'q' to quit"
		read a
		[ "$a" = "q" ] && exit 1
	fi
}

setup()
{
	set -e
	setup_ns ns1
	IP="$(which ip) -netns $ns1"
	NS_EXEC="$(which ip) netns exec $ns1"

	$IP link add dummy0 type dummy
	$IP link set dev dummy0 up
	$IP -6 address add 2001:db8:1::1/64 dev dummy0 nodad
	set +e
}

cleanup()
{
	$IP link del dev dummy0 &> /dev/null
	cleanup_ns $ns1
}

get_linklocal()
{
	local dev=$1
	local addr

	addr=$($IP -6 -br addr show dev ${dev} | \
	awk '{
		for (i = 3; i <= NF; ++i) {
			if ($i ~ /^fe80/)
				print $i
		}
	}'
	)
	addr=${addr/\/*}

	[ -z "$addr" ] && return 1

	echo $addr

	return 0
}

run_cmd()
{
	local cmd="$1"
	local out
	local stderr="2>/dev/null"

	if [ "$VERBOSE" = "1" ]; then
		printf "    COMMAND: $cmd\n"
		stderr=
	fi

	out=$(eval $cmd $stderr)
	rc=$?
	if [ "$VERBOSE" = "1" -a -n "$out" ]; then
		echo "    $out"
	fi

	[ "$VERBOSE" = "1" ] && echo

	return $rc
}

icmpv6_to_local_address()
{
	local rc

	echo
	echo "ICMPv6 to local addresses"

	setup

	local lldummy=$(get_linklocal dummy0)

	if [ -z "$lldummy" ]; then
		echo "Failed to get link local address for dummy0"
		return 1
	fi

	# ping6 to link local address
	run_cmd "$NS_EXEC ${ping6} -c 3 $lldummy%dummy0"
	log_test $? 0 "Ping to link local address"

	# ping6 to link local address from localhost (::1)
	run_cmd "$NS_EXEC ${ping6} -c 3 -I ::1 $lldummy%dummy0"
	log_test $? 0 "Ping to link local address from ::1"

	# ping6 to local address
	run_cmd "$NS_EXEC ${ping6} -c 3 2001:db8:1::1"
	log_test $? 0 "Ping to local address"

	# ping6 to local address from localhost (::1)
	run_cmd "$NS_EXEC ${ping6} -c 3 -I ::1 2001:db8:1::1"
	log_test $? 0 "Ping to local address from ::1"
}

################################################################################
# usage

usage()
{
	cat <<EOF
usage: ${0##*/} OPTS

    -t <test>   Test(s) to run (default: all)
                (options: $TESTS)
    -p          Pause on fail
    -P          Pause after each test before cleanup
    -v          Verbose mode (show commands and output)
EOF
}

################################################################################
# main

trap cleanup EXIT

while getopts :t:pPhv o
do
	case $o in
		t) TESTS=$OPTARG;;
		p) PAUSE_ON_FAIL=yes;;
		P) PAUSE=yes;;
		v) VERBOSE=$(($VERBOSE + 1));;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

[ "${PAUSE}" = "yes" ] && PAUSE_ON_FAIL=no

if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit $ksft_skip;
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool"
	exit $ksft_skip
fi

# start clean
cleanup &> /dev/null

for t in $TESTS
do
	case $t in
	icmpv6_to_local_address)	icmpv6_to_local_address;;

	help) echo "Test names: $TESTS"; exit 0;;
	esac
done

if [ "$TESTS" != "none" ]; then
	printf "\nTests passed: %3d\n" ${nsuccess}
	printf "Tests failed: %3d\n" ${nfail}
fi

exit $ret
