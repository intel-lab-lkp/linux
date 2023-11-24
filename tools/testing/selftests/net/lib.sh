#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

##############################################################################
# Defines

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
# namespace list created by setup_ns
NS_LIST=""

##############################################################################
# Helpers
busywait()
{
	local timeout=$1; shift

	local start_time="$(date -u +%s%3N)"
	while true
	do
		local out
		out=$($@)
		local ret=$?
		if ((!ret)); then
			echo -n "$out"
			return 0
		fi

		local current_time="$(date -u +%s%3N)"
		if ((current_time - start_time > timeout)); then
			echo -n "$out"
			return 1
		fi
	done
}

cleanup_ns()
{
	local ns=""
	local errexit=0

	# disable errexit temporary
	if [[ $- =~ "e" ]]; then
		errexit=1
		set +e
	fi

	for ns in "$@"; do
		ip netns delete "${ns}" &> /dev/null
		busywait 2 "ip netns list | grep -vq $1" &> /dev/null
		if ip netns list | grep -q $1; then
			echo "Failed to remove namespace $1"
			return $ksft_skip
		fi
	done

	[ $errexit -eq 1 ] && set -e
	return 0
}

# By default, remove all netns before EXIT.
cleanup_all_ns()
{
	cleanup_ns $NS_LIST
}
trap cleanup_all_ns EXIT

# setup netns with given names as prefix. e.g
# setup_ns local remote
setup_ns()
{
	local ns=""
	# the ns list we created in this call
	local ns_list=""
	while [ -n "$1" ]; do
		# Some test may setup/remove same netns multi times
		if unset $1 2> /dev/null; then
			ns="${1,,}-$(mktemp -u XXXXXX)"
			eval readonly $1=$ns
		else
			eval ns='$'$1
			cleanup_ns $ns

		fi

		ip netns add $ns
		if ! ip netns list | grep -q $ns; then
			echo "Failed to create namespace $1"
			cleanup_ns $ns_list
			return $ksft_skip
		fi
		ip -n $ns link set lo up
		ns_list="$ns_list $ns"

		shift
	done
	NS_LIST="$NS_LIST $ns_list"
}
