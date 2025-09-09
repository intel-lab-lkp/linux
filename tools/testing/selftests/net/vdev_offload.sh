#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# shellcheck disable=SC1091
source lib.sh

# Set related offload on lower deivces and check if upper device re-compute
# Some features are fixed on veth interface. Just list here in case we have a
# better way to test in future.
set_offload()
{
	local dev="$1"
	local state="$2"

	# VLAN features
	# NETIF_F_FRAGLIST: tx-scatter-gather-fraglist
	# shellcheck disable=SC2154
	ip netns exec "$ns" ethtool -K "$dev" tx-scatter-gather-fraglist "$state"

	# ENC features
	# NETIF_F_RXCSUM: rx-checksum (bond/team/bridge fixed)

	# XFRM features (veth fixed, netdevsim supports)
	# NETIF_F_HW_ESP: esp-hw-offload
	# NETIF_F_GSO_ESP: tx-esp-segmentation

	# GSO partial features
	# NETIF_F_GSO_PARTIAL: tx-gso-partial (veth/bond fixed)

	# Common features
	# NETIF_F_SG: tx-scatter-gather
	ip netns exec "$ns" ethtool -K "$dev" tx-scatter-gather "$state" &> /dev/null
	# NETIF_F_GSO_SOFTWARE: NETIF_F_GSO_ACCECN: tx-tcp-accecn-segmentation
	ip netns exec "$ns" ethtool -K "$dev" tx-tcp-accecn-segmentation "$state"
	# NETIF_F_GSO_SOFTWARE: NETIF_F_GSO_SCTP: tx-sctp-segmentation
	ip netns exec "$ns" ethtool -K "$dev" tx-sctp-segmentation "$state"
	# NETIF_F_GSO_SOFTWARE: NETIF_F_GSO_FRAGLIST: tx-gso-list
	ip netns exec "$ns" ethtool -K "$dev" tx-gso-list "$state"
}

__check_offload()
{
	local dev=$1
	local opt=$2
	local expect=$3

	ip netns exec "$ns" ethtool --json -k "$dev" | \
		jq -r -e ".[].\"$opt\".active == ${expect}" >/dev/null
}

check_offload()
{
	local dev=$1
	local state=$2

	__check_offload "$dev" "tx-scatter-gather-fraglist" "$state" || RET=1
	__check_offload "$dev" "tx-scatter-gather" "$state" || RET=1
	__check_offload "$dev" "tx-tcp-accecn-segmentation" "$state" || RET=1
	__check_offload "$dev" "tx-sctp-segmentation" "$state" || RET=1
	__check_offload "$dev" "tx-gso-list" "$state" || RET=1
}

setup_veth()
{
	# Set up test netns
	setup_ns ns switch

	# shellcheck disable=SC2154
	ip -n "$ns" link add veth0 type veth peer name veth0 netns "$switch"
	ip -n "$ns" link add veth1 type veth peer name veth1 netns "$switch"
	ip -n "$switch" link set veth0 up
	ip -n "$switch" link set veth1 up

	link_0=veth0
	link_1=veth1
}

cleanup()
{
	cleanup_all_ns
}

setup_bond()
{
	ip -n "$ns" link set "$link_0" nomaster
	ip -n "$ns" link set "$link_1" nomaster
	ip -n "$ns" link add bond0 type bond mode active-backup miimon 100
	ip -n "$ns" link set "$link_0" master bond0
	ip -n "$ns" link set "$link_1" master bond0
	ip -n "$ns" link set bond0 up
}

setup_team()
{
	ip -n "$ns" link set "$link_0" nomaster
	ip -n "$ns" link set "$link_1" nomaster
	ip -n "$ns" link add team0 type team
	ip -n "$ns" link set "$link_0" master team0
	ip -n "$ns" link set "$link_1" master team0
	ip -n "$ns" link set team0 up
}

setup_bridge()
{
	ip -n "$ns" link set "$link_0" nomaster
	ip -n "$ns" link set "$link_1" nomaster
	ip -n "$ns" link add br0 type bridge
	ip -n "$ns" link set "$link_0" master br0
	ip -n "$ns" link set "$link_1" master br0
	ip -n "$ns" link set br0 up
}

do_test()
{
	local dev=$1

	RET=0
	set_offload veth0 "on"
	set_offload veth1 "on"
	check_offload "$dev" "true"
	log_test "$dev" "enable offload"

	RET=0
	set_offload veth0 "off"
	set_offload veth1 "off"
	check_offload "$dev" "false"
	log_test "$dev" "disable offload"
}

trap cleanup EXIT
setup_veth
setup_bond
do_test bond0
setup_team
do_test team0
setup_bridge
do_test br0
