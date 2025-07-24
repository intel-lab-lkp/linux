#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Testing if bond lacp per port priority works

lib_dir=$(dirname "$0")
# shellcheck disable=SC1091
source "$lib_dir"/../../../net/lib.sh

# create client, switch, backup switch netns
setup_ns c_ns s_ns b_ns
defer cleanup_all_ns

# setup links
# shellcheck disable=SC2154
ip -n "${c_ns}" link add eth0 type veth peer name eth0 netns "${s_ns}"
ip -n "${c_ns}" link add eth1 type veth peer name eth1 netns "${s_ns}"
# shellcheck disable=SC2154
ip -n "${c_ns}" link add eth2 type veth peer name eth0 netns "${b_ns}"
ip -n "${c_ns}" link add eth3 type veth peer name eth1 netns "${b_ns}"

ip -n "${c_ns}" link add bond0 type bond mode 802.3ad miimon 100 lacp_rate fast ad_select prio
ip -n "${s_ns}" link add bond0 type bond mode 802.3ad miimon 100 lacp_rate fast
ip -n "${b_ns}" link add bond0 type bond mode 802.3ad miimon 100 lacp_rate fast

ip -n "${c_ns}" link set eth0 master bond0
ip -n "${c_ns}" link set eth1 master bond0
ip -n "${c_ns}" link set eth2 master bond0
ip -n "${c_ns}" link set eth3 master bond0
ip -n "${s_ns}" link set eth0 master bond0
ip -n "${s_ns}" link set eth1 master bond0
ip -n "${b_ns}" link set eth0 master bond0
ip -n "${b_ns}" link set eth1 master bond0

ip -n "${c_ns}" link set bond0 up
ip -n "${s_ns}" link set bond0 up
ip -n "${b_ns}" link set bond0 up

# set ad actor port priority, default 255
ip -n "${c_ns}" link set eth0 type bond_slave ad_actor_port_prio 1000
prio=$(cmd_jq "ip -n ${c_ns} -d -j link show eth0" ".[].linkinfo.info_slave_data.ad_actor_port_prio")
[ "$prio" -ne 1000 ] && RET=1
ip -n "${c_ns}" link set eth2 type bond_slave ad_actor_port_prio 10
prio=$(cmd_jq "ip -n ${c_ns} -d -j link show eth2" ".[].linkinfo.info_slave_data.ad_actor_port_prio")
[ "$prio" -ne 10 ] && RET=1
log_test "bond 802.3ad" "ad_actor_port_prio setting"

# Trigger link state change to reselect the aggregator
ip -n "${c_ns}" link set eth1 down
ip -n "${c_ns}" link set eth1 up
# the active agg should be connect to switch
bond_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show bond0" ".[].linkinfo.info_data.ad_info.aggregator")
eth0_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show eth0" ".[].linkinfo.info_slave_data.ad_aggregator_id")
if [ "${bond_agg_id}" -ne "${eth0_agg_id}" ]; then
	RET=1
fi

# Change the actor port prio and re-test
ip -n "${c_ns}" link set eth0 type bond_slave ad_actor_port_prio 10
ip -n "${c_ns}" link set eth2 type bond_slave ad_actor_port_prio 1000
# Trigger link state change to reselect the aggregator
ip -n "${c_ns}" link set eth1 down
ip -n "${c_ns}" link set eth1 up
# now the active agg should be connect to backup switch
bond_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show bond0" ".[].linkinfo.info_data.ad_info.aggregator")
eth2_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show eth2" ".[].linkinfo.info_slave_data.ad_aggregator_id")
# shellcheck disable=SC2034
if [ "${bond_agg_id}" -ne "${eth2_agg_id}" ]; then
	RET=1
fi
log_test "bond 802.3ad" "ad_actor_port_prio switch"

exit "${EXIT_STATUS}"
