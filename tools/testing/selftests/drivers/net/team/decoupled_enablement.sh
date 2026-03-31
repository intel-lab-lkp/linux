#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# These tests verify the decoupled RX and TX enablement of team driver member
# interfaces.
#
# Topology
#
#  +---------------------+  NS1
#  |      test_team1     |
#  |          |          |
#  |        eth0         |
#  |          |          |
#  |          |          |
#  +---------------------+
#             |
#  +---------------------+  NS2
#  |          |          |
#  |          |          |
#  |        eth0         |
#  |          |          |
#  |      test_team2     |
#  +---------------------+

ALL_TESTS="
	team_test_tx_enablement
	team_test_rx_enablement
"

test_dir="$(dirname "$0")"
source "${test_dir}/../../../net/lib.sh"

NS1=""
NS2=""
NODAD="nodad"
PREFIX_LENGTH="64"
NS1_IP="fd00::1"
NS2_IP="fd00::2"
NS1_IP4="192.168.0.1"
NS2_IP4="192.168.0.2"
MEMBERS=("eth0")
PING_COUNT=5
PING_TIMEOUT_S=1
PING_INTERVAL=0.1

while getopts "4" opt; do
	case $opt in
		4)
			echo "IPv4 mode selected."
			NODAD=
			PREFIX_LENGTH="24"
			NS1_IP="${NS1_IP4}"
			NS2_IP="${NS2_IP4}"
			;;
		\?)
			echo "Invalid option: -$OPTARG" >&2
			exit 1
			;;
	esac
done

# This has to be sourced after opts are gathered... (because of the forwarding
# lib)
source "${test_dir}/team_lib.sh"

# Create the network namespaces, veth pair, and team devices in the specified
# mode.
# Globals:
#   RET - Used by test infra, set by `check_err` functions.
# Arguments:
#   mode - The team driver mode to use for the team devices.
environment_create()
{
	trap cleanup_all_ns EXIT
	setup_ns ns1 ns2
	NS1="${NS_LIST[0]}"
	NS2="${NS_LIST[1]}"

	# Create the interfaces.
	ip -n "${NS1}" link add eth0 type veth peer name eth0 netns "${NS2}"
	ip -n "${NS1}" link add test_team1 type team
	ip -n "${NS2}" link add test_team2 type team

	# Set up the receiving network namespace's team interface.
	setup_team "${NS2}" test_team2 roundrobin "${NS2_IP}" \
			"${PREFIX_LENGTH}" "${MEMBERS[@]}"
}

set_option_value()
{
	local namespace="$1"
	local option_name="$2"
	local option_value="$3"
	local team_name="$4"
	local member_name="$5"
	local port_flag="--port=${member_name}"

	ip netns exec "${namespace}" teamnl "${team_name}" setoption \
			"${option_name}" "${option_value}" "${port_flag}"
	return $?
}

try_ping()
{
	ip netns exec "${NS1}" ping -i "${PING_INTERVAL}" -c "${PING_COUNT}" \
			"${NS2_IP}" -W "${PING_TIMEOUT_S}"
}

team_test_mode_tx_enablement()
{
	local mode="$1"
	RET=0

	# Set up the sender team with the correct mode.
	setup_team "${NS1}" test_team1 "${mode}" "${NS1_IP}" \
			"${PREFIX_LENGTH}" "${MEMBERS[@]}"
	check_err $? "Failed to set up sender team"

	### Scenario 1: Member interface initially enabled.
	# Expect ping to pass
	try_ping
	check_err $? "Ping failed when TX enabled"

	### Scenario 2: Once tx-side interface disabled.
	# Expect ping to fail.
	set_option_value "${NS1}" tx_enabled false test_team1 eth0
	check_err $? "Failed to disable TX"
	tcpdump_start eth0 "${NS2}"
	try_ping
	check_fail $? "Ping succeeded when TX disabled"
	tcpdump_stop eth0
	# Expect no packets to be transmitted, since TX is disabled.
	did_interface_receive_icmp eth0 "${NS2_IP}"
	check_fail $? "eth0 IS transmitting when TX disabled"

	### Scenario 3: The interface has tx re-enabled.
	# Expect ping to pass.
	set_option_value "${NS1}" tx_enabled true test_team1 eth0
	check_err $? "Failed to reenable TX"
	try_ping
	check_err $? "Ping failed when TX reenabled"

	log_test "TX failover of '${mode}' test"
}

team_test_mode_rx_enablement()
{
	local mode="$1"
	RET=0

	# Set up the sender team with the correct mode.
	setup_team "${NS1}" test_team1 "${mode}" "${NS1_IP}" \
			"${PREFIX_LENGTH}" "${MEMBERS[@]}"
	check_err $? "Failed to set up sender team"

	### Scenario 1: Member interface initially enabled.
	# Expect ping to pass
	try_ping
	check_err $? "Ping failed when RX enabled"

	### Scenario 2: Once rx-side interface disabled.
	# Expect ping to fail.
	set_option_value "${NS1}" rx_enabled false test_team1 eth0
	check_err $? "Failed to disable RX"
	tcpdump_start eth0 "${NS2}"
	try_ping
	check_fail $? "Ping succeeded when RX disabled"
	tcpdump_stop eth0
	# Expect packets to be transmitted, since only RX is disabled.
	did_interface_receive_icmp eth0 "${NS2_IP}"
	check_err $? "eth0 not transmitting when RX disabled"

	### Scenario 3: The interface has tx re-enabled.
	# Expect ping to pass.
	set_option_value "${NS1}" rx_enabled true test_team1 eth0
	check_err $? "Failed to reenable RX"
	try_ping
	check_err $? "Ping failed when RX reenabled"

	log_test "RX failover of '${mode}' test"
}

team_test_tx_enablement()
{
	team_test_mode_tx_enablement broadcast
	team_test_mode_tx_enablement roundrobin
	team_test_mode_tx_enablement random
}

team_test_rx_enablement()
{
	team_test_mode_rx_enablement broadcast
	team_test_mode_rx_enablement roundrobin
	team_test_mode_rx_enablement random
}

require_command teamnl ping pv
environment_create
tests_run
exit "${EXIT_STATUS}"
