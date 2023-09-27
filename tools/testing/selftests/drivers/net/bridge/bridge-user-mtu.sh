#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Ensure a bridge MTU does not automatically change when it has been specified
# by the user.
#
# To run independently:
# make TARGETS=drivers/net/bridge kselftest

ALL_TESTS="
	bridge_created_with_user_specified_mtu
	bridge_created_without_user_specified_mtu
	bridge_with_late_user_specified_mtu
"

REQUIRE_MZ=no
NUM_NETIFS=0
lib_dir=$(dirname "$0")
source "${lib_dir}"/net_forwarding_lib.sh

setup_prepare()
{
	for i in 1 3 5; do
		ip link add "vtest${i}" mtu 9000 type veth peer name "vtest${i}b" mtu 9000
	done
}

cleanup()
{
	for interface in vtest1 vtest3 vtest5 br-test0 br-test1 br-test2; do
		if [[ -d "/sys/class/net/${interface}" ]]; then
			ip link del "${interface}" &> /dev/null
		fi
	done
}

check_mtu()
{
	cur_mtu=$(<"/sys/class/net/$1/mtu")
	[[ ${cur_mtu} -eq $2 ]]
	exit_status=$?
	return "${exit_status}"
}

check_bridge_user_specified_mtu()
{
	if [[ -z $1 ]]
	then
		exit 1
	fi
	mtu=$1

	RET=0

	ip link add dev br-test0 mtu "${mtu}" type bridge
	ip link set br-test0 up
	check_mtu br-test0 "${mtu}"
	check_err $? "Bridge was not created with the user-specified MTU"

	check_mtu vtest1 9000
	check_err $? "vtest1 does not have MTU 9000"

	ip link set dev vtest1 master br-test0
	check_mtu br-test0 "${mtu}"
	check_err $? "Bridge user-specified MTU incorrectly changed after adding an interface"

	log_test "Bridge created with user-specified MTU (${mtu})"

	ip link del br-test0
}

bridge_created_with_user_specified_mtu() {
	# Check two user-specified MTU values
	# - 1500: To ensure the default MTU (1500) is not special-cased, you
	#         should be able to lock a bridge to the default MTU.
	# - 2000: Ensure bridges are actually created with a user-specified MTU
	check_bridge_user_specified_mtu 1500
	check_bridge_user_specified_mtu 2000
}

bridge_created_without_user_specified_mtu()
{
	RET=0
	ip link add dev br-test1 type bridge
	ip link set br-test1 up
	check_mtu br-test1 1500
	check_err $? "Bridge was not created with the user-specified MTU"

	ip link set dev vtest3 master br-test1
	check_mtu br-test1 9000
	check_err $? "Bridge without user-specified MTU did not change MTU"

	log_test "Bridge created without user-specified MTU"

	ip link del br-test1
}

check_bridge_late_user_specified_mtu()
{
	if [[ -z $1 ]]
	then
		exit 1
	fi
	mtu=$1

	RET=0
	ip link add dev br-test2 type bridge
	ip link set br-test2 up
	check_mtu br-test2 1500
	check_err $? "Bridge was not created with default MTU (1500)"

	ip link set br-test2 mtu "${mtu}"
	check_mtu br-test2 "${mtu}"
	check_err $? "User-specified MTU set after creation was not set"
	check_mtu vtest5 9000
	check_err $? "vtest5 does not have MTU 9000"

	ip link set dev vtest5 master br-test2
	check_mtu br-test2 "${mtu}"
	check_err $? "Bridge late-specified MTU incorrectly changed after adding an interface"

	log_test "Bridge created without user-specified MTU and changed after (${mtu})"

	ip link del br-test2
}

bridge_with_late_user_specified_mtu()
{
	# Note: Unfortunately auto-tuning is not disabled when you set the MTU
	# to it's current value, including the default of 1500. The reason is
	# that dev_set_mtu_ext skips notifying any handlers if the MTU is set
	# to the current value. Normally that makes sense, but is confusing
	# since you might expect "ip link set br0 mtu 1500" to lock the MTU to
	# 1500 but that will only happen if the MTU was not already 1500. So we
	# only check a non-default value of 2000 here unlike the earlier
	# bridge_created_with_user_specified_mtu test

	# Check one user-specified MTU value
	# - 2000: Ensure bridges actually change to a user-specified MTU
	check_bridge_late_user_specified_mtu 2000
}

trap cleanup EXIT

setup_prepare
tests_run

exit "${EXIT_STATUS}"
