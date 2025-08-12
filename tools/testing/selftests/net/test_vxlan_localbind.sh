#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This test is for checking the VXLAN localbind option.
#
# It simulates two hypervisors running a VM each using four network namespaces:
# two for the HVs, two for the VMs.
# A small VXLAN tunnel is made between the two hypervisors to have the two vms
# in the same virtual L2, connected through two separate subnets:
#
# +-------------------+                                    +-------------------+
# |                   |                                    |                   |
# |    vm-1 netns     |                                    |    vm-2 netns     |
# |                   |                                    |                   |
# |  +-------------+  |                                    |  +-------------+  |
# |  |   veth-hv   |  |                                    |  |   veth-hv   |  |
# |  | 10.0.0.1/24 |  |                                    |  | 10.0.0.2/24 |  |
# |  +-------------+  |                                    |  +-------------+  |
# |        .          |                                    |         .         |
# +-------------------+                                    +-------------------+
#          .                                                         .
#          .                                                         .
#          .                                                         .
# +-----------------------------------+   +------------------------------------+
# |        .                          |   |                          .         |
# |  +----------+                     |   |                     +----------+   |
# |  | veth-tap |                     |   |                     | veth-tap |   |
# |  +----+-----+                     |   |                     +----+-----+   |
# |       |                           |   |                          |         |
# |    +--+--+                        |   |                       +--+--+      |
# |    | br0 |                        |   |                       | br0 |      |
# |    +--+--+                        |   |                       +--+--+      |
# |       |                           |   |                          |         |
# |   +---+----+  +--------+--------+ |   | +--------+--------+  +---+----+    |
# |   | vxlan0 |..|      veth0      |.|...|.|      veth0      |..| vxlan0 |    |
# |   +--------+  | 172.16.1.1/24   | |   | | 172.16.1.2/24   |  +--------+    |
# |               | 172.16.2.1/24   | |   | | 172.16.2.2/24   |                |
# |               +-----------------+ |   | +-----------------+                |
# |                                   |   |                                    |
# |             hv-1 netns            |   |           hv-2 netns               |
# |                                   |   |                                    |
# +-----------------------------------+   +------------------------------------+
#
# This tests the connectivity between vm-1 and vm-2 using different subnet and
# localbind configurations.

source lib.sh
ret=0

TESTS="
    same_subnet
    same_subnet_localbind
    different_subnets
    different_subnets_localbind
"

VERBOSE=0
PAUSE_ON_FAIL=no
PAUSE=no

################################################################################
# Utilities

which ping6 > /dev/null 2>&1 && ping6=$(which ping6) || ping6=$(which ping)

log_test()
{
	local rc=$1
	local expected=$2
	local msg="$3"

	if [ ${rc} -eq ${expected} ]; then
		printf "TEST: %-60s  [ OK ]\n" "${msg}"
		nsuccess=$((nsuccess+1))
	else
		ret=1
		nfail=$((nfail+1))
		printf "TEST: %-60s  [FAIL]\n" "${msg}"
		if [ "$VERBOSE" = "1" ]; then
			echo "    rc=$rc, expected $expected"
		fi

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

	[ "$VERBOSE" = "1" ] && echo
}

run_cmd()
{
	local cmd="$1"
	local out
	local stderr="2>/dev/null"

	if [ "$VERBOSE" = "1" ]; then
		printf "COMMAND: $cmd\n"
		stderr=
	fi

	out=$(eval $cmd $stderr)
	rc=$?
	if [ "$VERBOSE" = "1" -a -n "$out" ]; then
		echo "    $out"
	fi

	return $rc
}

check_hv_connectivity() {
    slowwait 5 ip netns exec $hv_1 ping -c 1 -W 1 172.16.1.2 &>/dev/null
    slowwait 5 ip netns exec $hv_1 ping -c 1 -W 1 172.16.2.2 &>/dev/null

	return $?
}

check_vm_connectivity() {
    if [ $2 -eq 1 ]; then
        prefix="! "
    else
        prefix=""
    fi

	slowwait 5 run_cmd "${prefix}ip netns exec $vm_1 ping -c 1 -W 1 10.0.0.2"
	log_test $? 0 "VM connectivity over $1"
}

################################################################################
# Setup

setup-hv-networking() {
    id=$1
    local=$2
    remote=$3
    flags=$4

    [ $id -eq 1 ] && peer=2 || peer=1

    ip link set veth-hv-$id netns ${hv[$id]}
    ip -netns ${hv[$id]} link set veth-hv-$id name veth0
    ip -netns ${hv[$id]} link set veth0 up

    ip -netns ${hv[$id]} addr add 172.16.1.$id/24 dev veth0
    ip -netns ${hv[$id]} addr add 172.16.2.$id/24 dev veth0

    ip -netns ${hv[$id]} link add br0 type bridge
    ip -netns ${hv[$id]} link set br0 up

    ip -netns ${hv[$id]} link add vxlan0 type vxlan id 10 local 172.16.$local.$id remote 172.16.$remote.$peer $flags dev veth0 dstport 4789
    ip -netns ${hv[$id]} link set vxlan0 master br0
    ip -netns ${hv[$id]} link set vxlan0 up

    bridge -netns ${hv[$id]} fdb append 00:00:00:00:00:00 dev vxlan0 dst 172.16.$remote.$peer self permanent
}

setup-vm() {
    id=$1

    ip link add veth-tap type veth peer name veth-hv

    ip link set veth-tap netns ${hv[$id]}
    ip -netns ${hv[$id]} link set veth-tap master br0
    ip -netns ${hv[$id]} link set veth-tap up

    ip link set veth-hv address 02:1d:8d:dd:0c:6$id

    ip link set veth-hv netns ${vm[$id]}
    ip -netns ${vm[$id]} addr add 10.0.0.$id/24 dev veth-hv
    ip -netns ${vm[$id]} link set veth-hv up
}

setup()
{
    setup_ns hv_1 hv_2 vm_1 vm_2
    hv[1]=$hv_1
    hv[2]=$hv_2
    vm[1]=$vm_1
    vm[2]=$vm_2

    # Setup "Hypervisors" simulated with netns
    ip link add veth-hv-1 type veth peer name veth-hv-2
    setup-hv-networking 1 1 2 $2
    setup-hv-networking 2 $1 1 $2
    setup-vm 1
    setup-vm 2
}

cleanup() {
    ip link del veth-hv-1 2>/dev/null || true
    ip link del veth-tap 2>/dev/null || true

    cleanup_ns $hv_1 $hv_2 $vm_1 $vm_2
}

################################################################################
# Tests

same_subnet()
{
	setup 2 "nolocalbind"
    check_hv_connectivity
    check_vm_connectivity "same subnet (nolocalbind)" 0
}

same_subnet_localbind()
{
	setup 2 "localbind"
    check_hv_connectivity
    check_vm_connectivity "same subnet (localbind)" 0
}

different_subnets()
{
	setup 1 "nolocalbind"
    check_hv_connectivity
    check_vm_connectivity "different subnets (nolocalbind)" 0
}

different_subnets_localbind()
{
	setup 1 "localbind"
    check_hv_connectivity
    check_vm_connectivity "different subnets (localbind)" 1
}

################################################################################
# Usage

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
# Main

trap cleanup EXIT

while getopts ":t:pPvh" opt; do
	case $opt in
		t) TESTS=$OPTARG ;;
		p) PAUSE_ON_FAIL=yes;;
		P) PAUSE=yes;;
		v) VERBOSE=$(($VERBOSE + 1));;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

# Make sure we don't pause twice.
[ "${PAUSE}" = "yes" ] && PAUSE_ON_FAIL=no

if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit $ksft_skip;
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool"
	exit $ksft_skip
fi

if [ ! -x "$(command -v bridge)" ]; then
	echo "SKIP: Could not run test without bridge tool"
	exit $ksft_skip
fi

ip link help vxlan 2>&1 | grep -q "localbind"
if [ $? -ne 0 ]; then
	echo "SKIP: iproute2 ip too old, missing VXLAN localbind support"
	exit $ksft_skip
fi

cleanup

for t in $TESTS
do
	$t; cleanup;
done

if [ "$TESTS" != "none" ]; then
	printf "\nTests passed: %3d\n" ${nsuccess}
	printf "Tests failed: %3d\n"   ${nfail}
fi

exit $ret

