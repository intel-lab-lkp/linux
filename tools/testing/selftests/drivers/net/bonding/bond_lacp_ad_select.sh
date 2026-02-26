#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Testing if bond lacp per port priority works
#
#          Switch (s_ns)          Backup Switch (b_ns)
#  +-------------------------+ +-------------------------+
#  |          bond0          | |          bond0          |
#  |            +            | |            +            |
#  |      eth0  |  eth1      | |      eth0  |  eth1      |
#  |        +---+---+        | |        +---+---+        |
#  |        |       |        | |        |       |        |
#  +-------------------------+ +-------------------------+
#           |       |                   |       |
#  +-----------------------------------------------------+
#  |        |       |                   |       |        |
#  |        +-------+---------+---------+-------+        |
#  |      eth0     eth1       |       eth2     eth3      |
#  |                          +                          |
#  |                        bond0                        |
#  +-----------------------------------------------------+
#                        Client (c_ns)

lib_dir=$(dirname "$0")
# shellcheck disable=SC1091
source "$lib_dir"/../../../net/lib.sh

setup_links()
{
	# shellcheck disable=SC2154
	ip -n "${c_ns}" link add eth0 type veth peer name eth0 netns "${s_ns}"
	ip -n "${c_ns}" link add eth1 type veth peer name eth1 netns "${s_ns}"
	# shellcheck disable=SC2154
	ip -n "${c_ns}" link add eth2 type veth peer name eth0 netns "${b_ns}"
	ip -n "${c_ns}" link add eth3 type veth peer name eth1 netns "${b_ns}"

	ip -n "${c_ns}" link add bond0 type bond mode 802.3ad miimon 100 \
		lacp_rate fast ad_select actor_port_prio
	ip -n "${s_ns}" link add bond0 type bond mode 802.3ad miimon 100 \
		lacp_rate fast
	ip -n "${b_ns}" link add bond0 type bond mode 802.3ad miimon 100 \
		lacp_rate fast

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
}

test_port_prio_setting()
{
	RET=0
	ip -n "${c_ns}" link set eth0 type bond_slave actor_port_prio 1000
	prio=$(cmd_jq "ip -n ${c_ns} -d -j link show eth0" \
		".[].linkinfo.info_slave_data.actor_port_prio")
	[ "$prio" -ne 1000 ] && RET=1
	ip -n "${c_ns}" link set eth2 type bond_slave actor_port_prio 10
	prio=$(cmd_jq "ip -n ${c_ns} -d -j link show eth2" \
		".[].linkinfo.info_slave_data.actor_port_prio")
	[ "$prio" -ne 10 ] && RET=1
}

test_agg_reselect()
{
	local bond_agg_id slave_agg_id
	local expect_slave="$1"
	RET=0

	# Trigger link state change to reselect the aggregator
	ip -n "${c_ns}" link set eth1 down
	sleep 0.5
	ip -n "${c_ns}" link set eth1 up
	sleep 0.5

	bond_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show bond0" \
		".[].linkinfo.info_data.ad_info.aggregator")
	slave_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show $expect_slave" \
		".[].linkinfo.info_slave_data.ad_aggregator_id")
	# shellcheck disable=SC2034
	[ "${bond_agg_id}" -ne "${slave_agg_id}" ] && \
		RET=1
}

is_distributing()
{
	ip -j -n "$c_ns" -d link show "$1" \
		| jq -e '.[].linkinfo.info_slave_data.ad_actor_oper_port_state_str | index("distributing")' > /dev/null
}

get_churn_state()
{
	local slave=$1
	# shellcheck disable=SC2016
	ip netns exec "$c_ns" awk -v s="$slave" '
	$0 ~ "Slave Interface: " s {found=1}
	found && /Actor Churn State:/ { print $4; exit }
	' /proc/net/bonding/bond0
}

check_slave_state()
{
	local state=$1
	local slave_0=$2
	local slave_1=$3
	local churn_state
	RET=0

	s0_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show $slave_0" \
		".[].linkinfo.info_slave_data.ad_aggregator_id")
	s1_agg_id=$(cmd_jq "ip -n ${c_ns} -d -j link show $slave_1" \
		".[].linkinfo.info_slave_data.ad_aggregator_id")
	if [ "${s0_agg_id}" -ne "${s1_agg_id}" ]; then
		log_info "$state: $slave_0 $slave_1 agg ids are different"
		RET=1
	fi

	for s in "$slave_0" "$slave_1"; do
		churn_state=$(get_churn_state "$s")
		if [ "$state" = "active" ]; then
			if ! is_distributing "$s"; then
				log_info "$state: $s is not in distributing state"
				RET=1
			fi
			if [ "$churn_state" != "none" ]; then
				log_info "$state: $s churn state $churn_state"
				RET=1
			fi
		else
			# Backup state, should be in churned and not distributing
			if is_distributing "$s"; then
				log_info "$state: $s is in distributing state"
				RET=1
			fi
			if [ "$churn_state" != "churned" ]; then
				log_info "$state: $s churn state $churn_state"
				# shellcheck disable=SC2034
				RET=1
			fi
		fi
	done
}

trap cleanup_all_ns EXIT
setup_ns c_ns s_ns b_ns
setup_links

test_port_prio_setting
log_test "bond 802.3ad" "actor_port_prio setting"

test_agg_reselect eth0
log_test "bond 802.3ad" "actor_port_prio select"
# sleep for a while to make sure the mux state machine has completed.
sleep 10
check_slave_state active eth0 eth1
log_test "bond 802.3ad" "active state/churn checking"
# wait for churn timer expired, need a bit longer as we restart eth1
sleep 55
check_slave_state backup eth2 eth3
log_test "bond 802.3ad" "backup state/churn checking"

# Change the actor port prio and re-test
ip -n "${c_ns}" link set eth0 type bond_slave actor_port_prio 10
ip -n "${c_ns}" link set eth2 type bond_slave actor_port_prio 1000
test_agg_reselect eth2
log_test "bond 802.3ad" "actor_port_prio switch"
sleep 10
check_slave_state active eth2 eth3
log_test "bond 802.3ad" "active state/churn checking"
sleep 55
check_slave_state backup eth0 eth1
log_test "bond 802.3ad" "backup state/churn checking"

exit "${EXIT_STATUS}"
