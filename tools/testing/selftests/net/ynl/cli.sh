#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Test YNL CLI functionality

# shellcheck disable=SC1091,SC2034,SC2154,SC2317
source ../lib.sh

readonly NSIM_ID="1338"
readonly NSIM_DEV_NAME="nsim${NSIM_ID}"
readonly VETH_A="veth_a"
readonly VETH_B="veth_b"

ALL_TESTS="
	cli_list_families
	cli_netdev_ops
	cli_ethtool_ops
	cli_rt_ops
	cli_nlctrl_ops
"

# Test listing available families
cli_list_families()
{
	RET=0

	ynl --list-families &>/dev/null
	check_err $? "failed to list families"

	log_test "YNL CLI list families"
}

# Test netdev family operations (dev-get, queue-get)
cli_netdev_ops()
{
	RET=0

	local dev_output
	local ifindex
	ifindex=$(ip netns exec "$testns" cat /sys/class/net/"$NSIM_DEV_NAME"/ifindex)

	dev_output=$(ip netns exec "$testns" ynl --family netdev \
		--do dev-get --json "{\"ifindex\": $ifindex}" 2>/dev/null)
	check_err $? "failed to get netdev dev info for $NSIM_DEV_NAME"

	echo "$dev_output" | grep -q "ifindex"
	check_err $? "netdev dev-get output missing ifindex"

	ip netns exec "$testns" ynl --family netdev \
		--dump queue-get --json "{\"ifindex\": $ifindex}" &>/dev/null
	check_err $? "failed to get netdev queue info for $NSIM_DEV_NAME"

	log_test "YNL CLI netdev operations"
}

# Test ethtool family operations (rings-get, linkinfo-get)
cli_ethtool_ops()
{
	RET=0

	local rings_output
	rings_output=$(ip netns exec "$testns" ynl --family ethtool \
		--do rings-get --json "{\"header\": {\"dev-name\": \"$NSIM_DEV_NAME\"}}" 2>/dev/null)
	check_err $? "failed to get ethtool rings info for $NSIM_DEV_NAME"

	echo "$rings_output" | grep -q "header"
	check_err $? "ethtool rings-get output missing header"

	local linkinfo_output
	linkinfo_output=$(ip netns exec "$testns" ynl --family ethtool \
		--do linkinfo-get --json "{\"header\": {\"dev-name\": \"$VETH_A\"}}" 2>/dev/null)
	check_err $? "failed to get ethtool linkinfo for $VETH_A"

	echo "$linkinfo_output" | grep -q "header"
	check_err $? "ethtool linkinfo-get output missing header"

	log_test "YNL CLI ethtool operations"
}

# Test rt-* family operations (route, addr, link, neigh, rule)
cli_rt_ops()
{
	RET=0
	if ! ynl --list-families 2>/dev/null | grep -q "rt-"; then
		log_test_skip "YNL CLI rt-* operations" "no rt-* families available"
		return "$ksft_skip"
	fi

	local ifindex
	ifindex=$(ip netns exec "$testns" cat /sys/class/net/"$NSIM_DEV_NAME"/ifindex)

	if ynl --list-families 2>/dev/null | grep -q "rt-route"; then
		# Add route: 192.0.2.0/24 dev $dev scope link
		ip netns exec "$testns" ynl --family rt-route --do newroute --create \
			--json "{\"dst\": \"192.0.2.0\", \"oif\": $ifindex, \"rtm-dst-len\": 24, \"rtm-family\": 2, \"rtm-scope\": 253, \"rtm-type\": 1, \"rtm-protocol\": 3, \"rtm-table\": 254}" &>/dev/null

		local route_output
		route_output=$(ip netns exec "$testns" ynl --family rt-route \
			--dump getroute 2>/dev/null)
		check_err $? "failed to get route info"

		echo "$route_output" | grep -q "192.0.2.0"
		check_err $? "added route 192.0.2.0 not found in route output"

		ip netns exec "$testns" ynl --family rt-route --do delroute \
			--json "{\"dst\": \"192.0.2.0\", \"oif\": $ifindex, \"rtm-dst-len\": 24, \"rtm-family\": 2, \"rtm-scope\": 253, \"rtm-type\": 1, \"rtm-protocol\": 3, \"rtm-table\": 254}" &>/dev/null
	fi

	if ynl --list-families 2>/dev/null | grep -q "rt-addr"; then
		ip netns exec "$testns" ynl --family rt-addr --do newaddr \
			--json "{\"ifa-index\": $ifindex, \"local\": \"192.0.2.100\", \"ifa-prefixlen\": 24, \"ifa-family\": 2}" &>/dev/null

		local addr_output
		addr_output=$(ip netns exec "$testns" ynl --family rt-addr \
			--dump getaddr 2>/dev/null)
		check_err $? "failed to get address info"

		echo "$addr_output" | grep -q "192.0.2.100"
		check_err $? "added address 192.0.2.100 not found in address output"

		ip netns exec "$testns" ynl --family rt-addr --do deladdr \
			--json "{\"ifa-index\": $ifindex, \"local\": \"192.0.2.100\", \"ifa-prefixlen\": 24, \"ifa-family\": 2}" &>/dev/null
	fi

	if ynl --list-families 2>/dev/null | grep -q "rt-link"; then
		ip netns exec "$testns" ynl --family rt-link --do newlink --create \
			--json "{\"ifname\": \"dummy0\", \"linkinfo\": {\"kind\": \"dummy\"}}" &>/dev/null

		local link_output
		link_output=$(ip netns exec "$testns" ynl --family rt-link \
			--dump getlink 2>/dev/null)
		check_err $? "failed to get link info"

		echo "$link_output" | grep -q "$NSIM_DEV_NAME"
		check_err $? "test device not found in link output"

		echo "$link_output" | grep -q "dummy0"
		check_err $? "created dummy0 interface not found in link output"

		ip netns exec "$testns" ynl --family rt-link --do dellink \
			--json "{\"ifname\": \"dummy0\"}" &>/dev/null
	fi

	if ynl --list-families 2>/dev/null | grep -q "rt-neigh"; then
		# Add neighbor: 192.0.2.1 dev nsim1338 lladdr 11:22:33:44:55:66 PERMANENT
		ip netns exec "$testns" ynl --family rt-neigh --do newneigh --create \
			--json "{\"ndm-ifindex\": $ifindex, \"dst\": \"192.0.2.1\", \"lladdr\": \"11:22:33:44:55:66\", \"ndm-family\": 2, \"ndm-state\": 128}" &>/dev/null

		local neigh_output
		neigh_output=$(ip netns exec "$testns" ynl --family rt-neigh \
			--dump getneigh 2>/dev/null)
		check_err $? "failed to get neighbor info"

		echo "$neigh_output" | grep -q "192.0.2.1"
		check_err $? "added neighbor 192.0.2.1 not found in neighbor output"

		ip netns exec "$testns" ynl --family rt-neigh --do delneigh \
			--json "{\"ndm-ifindex\": $ifindex, \"dst\": \"192.0.2.1\", \"lladdr\": \"11:22:33:44:55:66\", \"ndm-family\": 2}" &>/dev/null
	fi

	if ynl --list-families 2>/dev/null | grep -q "rt-rule"; then
		# Add rule: from 192.0.2.0/24 lookup 100 none
		ip netns exec "$testns" ynl --family rt-rule --do newrule \
			--json "{\"family\": 2, \"src-len\": 24, \"src\": \"192.0.2.0\", \"table\": 100}" &>/dev/null

		local rule_output
		rule_output=$(ip netns exec "$testns" ynl --family rt-rule \
			--dump getrule 2>/dev/null)
		check_err $? "failed to get rule info"

		echo "$rule_output" | grep -q "192.0.2.0"
		check_err $? "added rule with src 192.0.2.0 not found in rule output"

		ip netns exec "$testns" ynl --family rt-rule --do delrule \
			--json "{\"family\": 2, \"src-len\": 24, \"src\": \"192.0.2.0\", \"table\": 100}" &>/dev/null
	fi

	log_test "YNL CLI rt-* operations"
}

# Test nlctrl family operations
cli_nlctrl_ops()
{
	RET=0

	local family_output
	family_output=$(ynl --family nlctrl \
		--do getfamily --json "{\"family-name\": \"netdev\"}" 2>/dev/null)
	check_err $? "failed to get nlctrl family info for netdev"

	echo "$family_output" | grep -q "family-name"
	check_err $? "nlctrl getfamily output missing family-name"

	echo "$family_output" | grep -q "family-id"
	check_err $? "nlctrl getfamily output missing family-id"

	log_test "YNL CLI nlctrl getfamily"
}

setup()
{
	if ! modprobe netdevsim &>/dev/null; then
		log_test_skip "all YNL CLI tests" "netdevsim module not available"
		exit "$ksft_skip"
	fi

	setup_ns testns

	if ! create_netdevsim "$NSIM_ID" "$testns" &>/dev/null; then
		log_test_skip "all YNL CLI tests" "failed to create netdevsim device"
		exit "$ksft_skip"
	fi

	if ! ip -n "$testns" link add "$VETH_A" type veth peer name "$VETH_B"; then
		log_test_skip "all YNL CLI tests" "failed to create veth pair"
		exit "$ksft_skip"
	fi

	ip -n "$testns" link set "$VETH_A" up
	ip -n "$testns" link set "$VETH_B" up
}

cleanup()
{
	cleanup_netdevsim "$NSIM_ID"
	cleanup_all_ns
}

trap cleanup EXIT

require_command "ynl"
setup
tests_run

exit "$EXIT_STATUS"
