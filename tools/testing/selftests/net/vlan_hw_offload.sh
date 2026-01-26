#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2329

setup() {
	ip link add veth0 type veth peer name veth1
	ip link set veth0 up
	ip link set veth1 up
}

cleanup() {
	ip link delete veth0 2>/dev/null
}

# turn on hw offload and set up vlan dev with reorder_hdr off
test_vlan_hw_offload_toggle_crash() {
	ethtool -K veth0 tx-vlan-hw-insert off
	ip link add link veth0 name veth0.10 type vlan id 10 reorder_hdr off
	ethtool -K veth0 tx-vlan-hw-insert on

	# set up vlan dev and it will trigger ndisc
	ip link set veth0.10 up
	ip -6 route show dev veth0.10
}

trap cleanup EXIT

setup
test_vlan_hw_offload_toggle_crash

exit 0
