#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# Tests extension header limits.
#
# We start by setting up two network namespaces with IPv6 addresses.
# Then we create ICMPv6 Echo Request packets with various combinations of
# Extension Headers, and send them from one namespace to the other and
# check if a reply is received. Based on the sysctl settings certain packets
# are expected to produce echo replies and others are expected to be drop
# because an Extension Header related limit is exceeded. If an Echo Reply is
# received or not received per our expectations then the test passes,
# otherwise if the result is unexpected that's a test failure.
# Tests extension header limits.

source lib.sh

# all tests in this script. Can be overridden with -t option
TESTS="eh_limits"

VERBOSE=""
PAUSE_ON_FAIL=no
PAUSE=no
NAME="EH-limits"

IP1="2001:db8::1"
IP2="2001:db8::2"

log_test()
{
	local rc=$1
	local expected=$2
	local msg="$3"

	if [ "${rc}" -eq "${expected}" ]; then
		printf "    TEST: %-60s  [ OK ]\n" "${msg}"
		nsuccess=$((nsuccess+1))
	else
		nfail=$((nfail+1))
		printf "    TEST: %-60s  [FAIL]\n" "${msg}"
		if [ "${PAUSE_ON_FAIL}" = "yes" ]; then
		echo
			echo "hit enter to continue, 'q' to quit"
			read -r a
			[ "$a" = "q" ] && exit 1
		fi
	fi

	if [ "${PAUSE}" = "yes" ]; then
		echo
		echo "hit enter to continue, 'q' to quit"
		read -r a
		[ "$a" = "q" ] && exit 1
	fi
}

################################################################################
# Setup

setup()
{
	set -e

	setup_ns ns1 ns2

	NS_EXEC="ip netns exec"

	ip link add veth1 type veth peer name veth2

	ip link set veth1 netns "$ns1"
	ip link set veth2 netns "$ns2"

	$NS_EXEC "$ns1" ip addr add ${IP1}/64 dev veth1
	$NS_EXEC "$ns1" ip link set veth1 up
	$NS_EXEC "$ns1" ip link set lo up

	$NS_EXEC "$ns2" ip addr add ${IP2}/64 dev veth2
	$NS_EXEC "$ns2" ip link set veth2 up
	$NS_EXEC "$ns2" ip link set lo up

	# Enable SRv6 on the receiver since that's the type of routing header
	# used in the test
	$NS_EXEC "$ns2" sysctl -w net.ipv6.conf.all.seg6_enabled=1 > /dev/null
	$NS_EXEC "$ns2" sysctl -w net.ipv6.conf.veth2.seg6_enabled=1 > /dev/null

	set +e

	# Send a ping to do neighuor discovery
	$NS_EXEC "$ns1" ping6 -w 2 $IP2 -c 1 > /dev/null
}

exit_cleanup_all()
{
	cleanup_all_ns
	exit "${EXIT_STATUS}"
}

eh_limits_test()
{
	local ip_addrs="--src_ip $IP1 --dst_ip $IP2"

	# Note that we can't double quote $ip_addrs below to prevent globbing.
	# This is not an issues since ip_addrs is always set. Similarly, don't
	# double quote $VERBOSE in the exec commands, if it's empty then that's
	# okay

	if [ "$VERBOSE" = "-v" ]; then
		echo ">>>>> Default"
	fi

	# Run the test with default sysctl settings
	$NS_EXEC "$ns1" python3 ./eh_limits.py $VERBOSE $ip_addrs
	$NS_EXEC "$ns1" python3 ./eh_limits.py $ip_addrs

	log_test $? 0 "$NAME - default sysctls"

	if [ "$VERBOSE" = "-v" ]; then
		echo ">>>>> No order enforce, 8 options, 66 length limit"
	fi

	# Set extension header limit sysctls. We do this on both sides since
	# the sender reads the sysctl's to determine pass/fail expectations

	$NS_EXEC "$ns1" sysctl -w net.ipv6.enforce_ext_hdr_order=0 > /dev/null
	$NS_EXEC "$ns1" sysctl -w net.ipv6.max_dst_opts_number=8 > /dev/null
	$NS_EXEC "$ns1" sysctl -w net.ipv6.max_hbh_opts_number=8 > /dev/null
	$NS_EXEC "$ns1" sysctl -w net.ipv6.max_hbh_length=64 > /dev/null
	$NS_EXEC "$ns1" sysctl -w net.ipv6.max_dst_opts_length=64 > /dev/null

	$NS_EXEC "$ns2" sysctl -w net.ipv6.enforce_ext_hdr_order=0 > /dev/null
	$NS_EXEC "$ns2" sysctl -w net.ipv6.max_dst_opts_number=8 > /dev/null
	$NS_EXEC "$ns2" sysctl -w net.ipv6.max_hbh_opts_number=8 > /dev/null
	$NS_EXEC "$ns2" sysctl -w net.ipv6.max_hbh_length=64 > /dev/null
	$NS_EXEC "$ns2" sysctl -w net.ipv6.max_dst_opts_length=64 > /dev/null

	# Run the test with modified sysctl settings
	$NS_EXEC "$ns1" python3 ./eh_limits.py $VERBOSE $ip_addrs

	log_test $? 0 "$NAME - modified sysctls"
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
        -v          verbose mode (show commands and output)
EOF
}

################################################################################
# main

require_command scapy

while getopts :t:pPhv o
do
	case $o in
		t) TESTS=$OPTARG;;
		p) PAUSE_ON_FAIL=yes;;
		P) PAUSE=yes;;
		v) VERBOSE="-v";;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

# make sure we don't pause twice
[ "${PAUSE}" = "yes" ] && PAUSE_ON_FAIL=no

ksft_skip=4

if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit "$ksft_skip"
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool"
	exit "$ksft_skip"
fi

if [ ! -x "$(command -v socat)" ]; then
	echo "SKIP: Could not run test without socat tool"
	exit "$ksft_skip"
fi

# start clean
cleanup &> /dev/null

for t in $TESTS
do
	case $t in
	eh_limits)		setup; eh_limits_test; cleanup_all_ns;;

	help) echo "Test names: $TESTS"; exit 0;;
	esac
done

if [ "$TESTS" != "none" ]; then
	printf "\nTests passed: %3d\n" ${nsuccess}
	printf "Tests failed: %3d\n"   ${nfail}
fi
