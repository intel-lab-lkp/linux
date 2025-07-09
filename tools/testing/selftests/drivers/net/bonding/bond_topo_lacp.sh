#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Topology for Bond mode 802.3ad testing
#
#  +-------------------------+
#  |          bond0          |  Switch
#  |            +            |  192.0.2.254/24
#  |      eth0  |  eth1      |  2001:db8::254/24
#  |        +---+---+        |
#  |        |       |        |
#  +-------------------------+
#           |       |
#  +-------------------------+
#  |        |       |        |
#  |        +---+---+        |  Client
#  |      eth0  |  eth1      |  192.0.2.1/24
#  |            +            |  2001:db8::1/24
#  |          bond0          |
#  +-------------------------+

REQUIRE_MZ=no
NUM_NETIFS=0
lib_dir=$(dirname "$0")
source "$lib_dir"/../../../net/forwarding/lib.sh

s_ip4="192.0.2.254"
c_ip4="192.0.2.1"
s_ip6="2001:db8::254"
c_ip6="2001:db8::1"

switch_create()
{
	ip netns exec ${s_ns} sysctl -q net.ipv6.conf.default.keep_addr_on_down=1
	ip -n ${s_ns} link add eth0 type veth peer name eth0 netns ${c_ns}
	ip -n ${s_ns} link add eth1 type veth peer name eth1 netns ${c_ns}
	ip -n ${s_ns} link add bond0 type bond mode 802.3ad miimon 100 lacp_active on lacp_rate fast
	ip -n ${s_ns} link set eth0 master bond0
	ip -n ${s_ns} link set eth1 master bond0
	ip -n ${s_ns} addr add ${s_ip4}/24 dev bond0
	ip -n ${s_ns} addr add ${s_ip6}/24 dev bond0
	ip -n ${s_ns} link set bond0 up
}

client_create()
{
	ip netns exec ${c_ns} sysctl -q net.ipv6.conf.default.keep_addr_on_down=1
	ip -n ${c_ns} link add bond0 type bond mode 802.3ad miimon 100 lacp_active on lacp_rate fast
	ip -n ${c_ns} link set eth0 master bond0
	ip -n ${c_ns} link set eth1 master bond0
	ip -n ${c_ns} addr add ${c_ip4}/24 dev bond0
	ip -n ${c_ns} addr add ${c_ip6}/24 dev bond0
	ip -n ${c_ns} link set bond0 up
}

setup_topo_lacp()
{
	setup_ns s_ns c_ns
	defer cleanup_all_ns

	switch_create
	client_create
}

# Reset bond with and options
lacp_bond_reset()
{
	local netns="$1"
	local param="$2"

	ip -n ${netns} link set bond0 down
	ip -n ${netns} link set bond0 type bond $param
	ip -n ${netns} link set bond0 up

	# Wait for IPv6 address ready as it needs DAD
	slowwait 10 ip netns exec ${c_ns} ping6 ${s_ip6} -c 1 -W 0.1 &> /dev/null
}
