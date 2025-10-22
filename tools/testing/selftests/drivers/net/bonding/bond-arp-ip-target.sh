#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test bonding arp_ip_target.
# Topology for Bond mode 1,5,6 testing
#
#  +-------------------------+
#  |                         | Server
#  |        bond0.10.20      | 192.20.2.1/24
#  |            |            |
#  |         bond0.10        | 192.10.2.1/24
#  |            |            |
#  |          bond0          | 192.0.2.1/24
#  |            |            |
#  |            +            |
#  |      eth0  |  eth1      |
#  |        +---+---+        |
#  |        |       |        |
#  +-------------------------+
#           |       |
#  +-------------------------+
#  |        |       |        |
#  |    +---+-------+---+    |  Gateway
#  |    |      br0      |    |
#  |    +-------+-------+    |
#  |            |            |
#  +-------------------------+
#               |
#  +-------------------------+
#  |            |            |  Client
#  |          eth0           | 192.0.0.2/24
#  |            |            |
#  |         eth0.10         | 192.10.10.2/24
#  |            |            |
#  |        eth0.10.20       | 192.20.20.2/24
#  +-------------------------+

# shellcheck disable=SC2317

lib_dir=$(dirname "$0")

# shellcheck source=/dev/null # Ignore source warning.
source "${lib_dir}"/bond_topo_2d1c.sh

# shellcheck disable=SC2154 # Ignore unassigned referenced warning.
echo "${c_ns}" "${s_ns}" > /dev/null

DEBUG=${DEBUG:-0}
test "${DEBUG}" -ne 0 && set -x

# vlan subnets
c_ip4="192.0.2.10"
c_ip4v10="192.10.2.10"
c_ip4v20="192.20.2.10"

export ALL_TESTS="
	no_vlan_hints
	with_vlan_hints
"

# Build stacked vlans on top of an interface.
stack_vlans()
{
	RET=0
	local interface="$1"
	local ns=$2
	local last="$interface"
	local tags="10 20"

	if ! ip -n "${ns}" link show "${interface}" > /dev/null; then
		RET=1
		msg="Failed to create ${interface}"
		return 1
	fi

	if [ "$ns" == "${s_ns}" ]; then host=1; else host=10;fi

	for tag in $tags; do
		ip -n "${ns}" link add link "$last" name "$last"."$tag" type vlan id "$tag"
		ip -n "${ns}" address add 192."$tag".2."$host"/24 dev "$last"."$tag"
		ip -n "${ns}" link set up dev "$last"."$tag"
		last=$last.$tag
	done
}

wait_for_arp_request()
{
	local target=$1
	local ip
	local interface

	ip=$(echo "${target}" | awk -F "[" '{print $1}')
	interface="$(ip -n "${c_ns}" -br addr show | grep "${ip}" | awk -F @ '{print $1}')"

	tc -n "${c_ns}" qdisc add dev "${interface}" clsact
	tc -n "${c_ns}" filter add dev "${interface}" ingress protocol arp \
	handle 101 flower skip_hw arp_op request arp_tip "${ip}" action pass

	slowwait_for_counter 5 5 tc_rule_handle_stats_get \
		"dev ${interface} ingress" 101 ".packets" "-n ${c_ns}" &> /dev/null || RET=1

	tc -n "${c_ns}" filter del dev "${interface}" ingress
	tc -n "${c_ns}" qdisc del dev "${interface}" clsact

	if [ "$RET" -ne 0 ]; then
		msg="Arp probe not received by ${interface}"
		return 1
	fi
}

# Check for link flapping.
# First verify the arp requests are being received
# by the target.  Then verify that the Link Failure
# Counts are not increasing over time.
# Arp probes are sent every 100ms, two probes must
# be missed to trigger a slave failure. A one second
# wait should be sufficient.
check_failure_count()
{
	local bond=$1
	local target=$2
	local proc_file=/proc/net/bonding/${bond}

	wait_for_arp_request "${target}" || return 1

	LinkFailureCount1=$(ip netns exec "${s_ns}" grep -F "Link Failure Count" "${proc_file}" \
	| awk -F: '{ sum += $2 } END { print sum }')
	sleep 1
	LinkFailureCount2=$(ip netns exec "${s_ns}" grep -F "Link Failure Count" "${proc_file}" \
	| awk -F: '{ sum += $2 } END { print sum }')

	[ "$LinkFailureCount1" != "$LinkFailureCount2" ] && RET=1
}

setup_bond_topo()
{
	setup_prepare
	setup_wait
	stack_vlans bond0 "${s_ns}"
	stack_vlans eth0 "${c_ns}"
}

skip_with_vlan_hints()
{
	# check if iproute supports arp_ip_target with vlans option.
	if ! ip -n "${s_ns}" link add bond2 type bond arp_ip_target 10.0.0.1[10]; then
		ip -n "${s_ns}" link del bond2 2> /dev/null
		return 0
	fi
	return 1
}

no_vlan_hints()
{
	RET=0
	local targets="${c_ip4} ${c_ip4v10} ${c_ip4v20}"
	local target
	msg=""

	for target in $targets; do
		bond_reset "mode $mode arp_interval 100 arp_ip_target ${target}"
		stack_vlans bond0 "${s_ns}"
		if [ "$RET" -ne 0 ]; then
			log_test "no_vlan_hints" "${msg}"
			return
		fi
		check_failure_count bond0 "${target}"
		log_test "arp_ip_target=${target} ${msg}"
	done
}

with_vlan_hints()
{
	RET=0
	local targets="${c_ip4}[] ${c_ip4v10}[10] ${c_ip4v20}[10/20]"
	local target
	msg=""

	if skip_with_vlan_hints; then
		log_test_skip "skip_with_vlan_hints" \
		"Installed iproute doesn't support extended arp_ip_target options."
		return 0
	fi

	for target in $targets; do
		bond_reset "mode $mode arp_interval 100 arp_ip_target ${target}"
		stack_vlans bond0 "${s_ns}"
		if [ "$RET" -ne 0 ]; then
			log_test "no_vlan_hints" "${msg}"
			return
		fi
		check_failure_count bond0 "${target}"
		log_test "arp_ip_target=${target} ${msg}"
	done
}

trap cleanup EXIT

mode=active-backup

setup_bond_topo
tests_run

exit "$EXIT_STATUS"
