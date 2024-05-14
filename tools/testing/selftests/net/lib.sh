#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

##############################################################################
# Defines

WAIT_TIMEOUT=${WAIT_TIMEOUT:=20}
BUSYWAIT_TIMEOUT=$((WAIT_TIMEOUT * 1000)) # ms

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
		out=$("$@")
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

# timeout in seconds
slowwait()
{
	local timeout=$1; shift

	local start_time="$(date -u +%s)"
	while true
	do
		local out
		out=$("$@")
		local ret=$?
		if ((!ret)); then
			echo -n "$out"
			return 0
		fi

		local current_time="$(date -u +%s)"
		if ((current_time - start_time > timeout)); then
			echo -n "$out"
			return 1
		fi

		sleep 0.1
	done
}

until_counter_is()
{
	local expr=$1; shift
	local current=$("$@")

	echo $((current))
	((current $expr))
}

busywait_for_counter()
{
	local timeout=$1; shift
	local delta=$1; shift

	local base=$("$@")
	busywait "$timeout" until_counter_is ">= $((base + delta))" "$@"
}

slowwait_for_counter()
{
	local timeout=$1; shift
	local delta=$1; shift

	local base=$("$@")
	slowwait "$timeout" until_counter_is ">= $((base + delta))" "$@"
}

cleanup_ns()
{
	local ns=""
	local errexit=0
	local ret=0

	# disable errexit temporary
	if [[ $- =~ "e" ]]; then
		errexit=1
		set +e
	fi

	for ns in "$@"; do
		ip netns delete "${ns}" &> /dev/null
		if ! busywait $BUSYWAIT_TIMEOUT ip netns list \| grep -vq "^$ns$" &> /dev/null; then
			echo "Warn: Failed to remove namespace $ns"
			ret=1
		fi
	done

	[ $errexit -eq 1 ] && set -e
	return $ret
}

cleanup_all_ns()
{
	cleanup_ns $NS_LIST
}

# setup netns with given names as prefix. e.g
# setup_ns local remote
setup_ns()
{
	local ns=""
	local ns_name=""
	local ns_list=""
	for ns_name in "$@"; do
		# Some test may setup/remove same netns multi times
		if unset ${ns_name} 2> /dev/null; then
			ns="${ns_name,,}-$(mktemp -u XXXXXX)"
			eval readonly ${ns_name}="$ns"
		else
			eval ns='$'${ns_name}
			cleanup_ns "$ns"

		fi

		if ! ip netns add "$ns"; then
			echo "Failed to create namespace $ns_name"
			cleanup_ns "$ns_list"
			return $ksft_skip
		fi
		ip -n "$ns" link set lo up
		ns_list="$ns_list $ns"
	done
	NS_LIST="$NS_LIST $ns_list"
}

tc_rule_stats_get()
{
	local dev=$1; shift
	local pref=$1; shift
	local dir=$1; shift
	local selector=${1:-.packets}; shift

	tc -j -s filter show dev $dev ${dir:-ingress} pref $pref \
	    | jq ".[1].options.actions[].stats$selector"
}

tc_rule_handle_stats_get()
{
	local id=$1; shift
	local handle=$1; shift
	local selector=${1:-.packets}; shift
	local netns=${1:-""}; shift

	tc $netns -j -s filter show $id \
	    | jq ".[] | select(.options.handle == $handle) | \
		  .options.actions[0].stats$selector"
}
