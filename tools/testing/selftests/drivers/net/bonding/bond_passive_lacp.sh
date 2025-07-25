#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test if a bond interface works with lacp_active=off.

# shellcheck disable=SC2034
REQUIRE_MZ=no
NUM_NETIFS=0
lib_dir=$(dirname "$0")
# shellcheck disable=SC1091
source "$lib_dir"/../../../net/forwarding/lib.sh

check_port_state()
{
	local netns=$1
	local port=$2
	local state=$3

	ip -n "${netns}" -d -j link show "$port" | \
		jq -e ".[].linkinfo.info_slave_data.ad_actor_oper_port_state_str | index(\"${state}\") != null" > /dev/null
}

trap cleanup EXIT
setup_ns c_ns s_ns
defer cleanup_all_ns

# shellcheck disable=SC2154
ip -n "${c_ns}" link add eth0 type veth peer name eth0 netns "${s_ns}"
ip -n "${c_ns}" link add eth1 type veth peer name eth1 netns "${s_ns}"
ip -n "${s_ns}" link set eth0 up
ip -n "${s_ns}" link set eth1 up
ip -n "${c_ns}" link add bond0 type bond mode 802.3ad lacp_active off lacp_rate fast
ip -n "${c_ns}" link set eth0 master bond0
ip -n "${c_ns}" link set eth1 master bond0
ip -n "${c_ns}" link set bond0 up

# 1. The passive side shouldn't send LACPDU.
RET=0
client_mac=$(cmd_jq "ip -j -n ${c_ns} link show bond0" ".[].address")
# Wait for the first LACPDU due to state change.
sleep 2
timeout 62 ip netns exec "${c_ns}" tcpdump --immediate-mode -c 1 -i eth0 \
	-nn -l -vvv ether proto 0x8809 2> /dev/null > /tmp/client_init.out
grep -q "System $client_mac" /tmp/client_init.out && RET=1
log_test "802.3ad" "init port pkt lacp_active off"

# 2. The passive side should not have the 'active' flag.
RET=0
check_port_state "${c_ns}" "eth0" "active" && RET=1
log_test "802.3ad" "port state lacp_active off"

# Set up the switch side with active mode.
ip -n "${s_ns}" link set eth0 down
ip -n "${s_ns}" link set eth1 down
ip -n "${s_ns}" link add bond0 type bond mode 802.3ad lacp_active on lacp_rate fast
ip -n "${s_ns}" link set eth0 master bond0
ip -n "${s_ns}" link set eth1 master bond0
ip -n "${s_ns}" link set bond0 up

# 3. The active side should have the 'active' flag.
RET=0
check_port_state "${s_ns}" "eth0" "active" || RET=1
log_test "802.3ad" "port state lacp_active on"

# 4. Make sure the connection has not expired.
RET=0
slowwait 15 check_port_state "${s_ns}" "eth0" "expired" && RET=1
slowwait 15 check_port_state "${s_ns}" "eth1" "expired" && RET=1
log_test "bond 802.3ad" "port connect lacp_active off"

# After testing, disconnect one port on each side to check the state.
ip -n "${s_ns}" link set eth0 nomaster
ip -n "${s_ns}" link set eth0 up
ip -n "${c_ns}" link set eth1 nomaster
ip -n "${c_ns}" link set eth1 up
# 5. The passive side shouldn't send LACPDU anymore.
RET=0
# Wait for LACPDU due to state change.
sleep 5
timeout 62 ip netns exec "${c_ns}" tcpdump --immediate-mode -c 1 -i eth0 \
	-nn -l -vvv ether proto 0x8809 2> /dev/null > /tmp/client_dis.out
grep -q "System $client_mac" /tmp/client_dis.out && RET=1
log_test "bond 802.3ad" "disconnect port pkt lacp_active off"

# 6. The active side keeps sending LACPDU.
RET=0
switch_mac=$(cmd_jq "ip -j -n ${s_ns} link show bond0" ".[].address")
timeout 62 ip netns exec "${s_ns}" tcpdump --immediate-mode -c 1 -i eth1 \
	-nn -l -vvv ether proto 0x8809 2> /dev/null > /tmp/switch.out
grep -q "System $switch_mac" /tmp/switch.out || RET=1
log_test "bond 802.3ad" "disconnect port pkt lacp_active on"

exit "$EXIT_STATUS"
