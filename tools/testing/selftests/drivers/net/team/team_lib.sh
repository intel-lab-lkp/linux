#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

REQUIRE_MZ=no
NUM_NETIFS=0
source "${test_dir}/../../../net/forwarding/lib.sh"

# Create a team interface inside of a given network namespace with a given
# mode, members, and IP address.
# Arguments:
#  namespace - Network namespace to put the team interface into.
#  team - The name of the team interface to setup.
#  mode - The team mode of the interface.
#  ip_address - The IP address to assign to the team interface.
#  prefix_length - The prefix length for the IP address subnet.
#  $@ - members - The member interfaces of the aggregation.
setup_team()
{
	local namespace=$1
	local team=$2
	local mode=$3
	local ip_address=$4
	local prefix_length=$5
	shift 5
	local members=("$@")

	# Prerequisite: team must have no members
	for member in "${members[@]}"; do
		ip -n "${namespace}" link set "${member}" nomaster
	done

	# Prerequisite: team must have no address in order to set it
	ip -n "${namespace}" addr del "${ip_address}/${prefix_length}" \
			${NODAD} dev "${team}"

	echo "Setting team in ${namespace} to mode ${mode}"

	if ! ip -n "${namespace}" link set "${team}" down; then
		echo "Failed to bring team device down"
		return 1
	fi
	if ! ip netns exec "${namespace}" teamnl "${team}" setoption mode \
			"${mode}"; then
		echo "Failed to set ${team} mode to '${mode}'"
		return 1
	fi

	# Aggregate the members into teams.
	for member in "${members[@]}"; do
		ip -n "${namespace}" link set ${member} master "${team}"
	done

	# Bring team devices up and give them addresses.
	if ! ip -n "${namespace}" link set "${team}" up; then
		echo "Failed to set ${team} up"
		return 1
	fi

	if ! ip -n ${namespace} addr add "${ip_address}/${prefix_length}" \
			${NODAD} dev "${team}"; then
		echo "Failed to give ${team} IP address in ${namespace}"
		return 1
	fi
}

# This is global used to keep track of the sender's netcat process, so that it
# can be terminated.
declare sender_pid

# Start sending and receiving TCP traffic with netcat.
# Globals:
#  sender_pid - The process ID of the netcat sender process. Used to kill it
#  later.
start_listening_and_sending()
{
	ip netns exec "${NS2}" nc -l "${NS2_IP}" 1234 > /dev/null &
	ip netns exec "${NS1}" /bin/bash -c "cat /dev/urandom | pv -q -L 1m | \
			nc ${NS2_IP} 1234 &"
	sender_pid=$!
}

# Stop sending TCP traffic with netcat.
# Globals:
#   sender_pid - The process IF of the netcat sender process.
stop_sending_and_listening()
{
	kill "${sender_pid}" && wait "${sender_pid}"
}

# Monitor for TCP traffic with Tcpdump, save results to temp files.
# Arguments:
#   namespace - The network namespace to run tcpdump inside of.
#   $@ - interfaces - The interfaces to listen to.
save_tcpdump_outputs()
{
	local namespace=$1
	shift 1
	local interfaces=("$@")

	for interface in "${interfaces[@]}"; do
		tcpdump_start_nosleep "${interface}" "${namespace}"
	done

	sleep 1

	for interface in "${interfaces[@]}"; do
		tcpdump_stop_nosleep "${interface}"
	done
}

# Read Tcpdump output, determine packet counts.
# Arguments:
#   interface - The name of the interface to count packets for.
#   ip_address - The destination IP address.
did_interface_receive()
{
	local interface="$1"
	local ip_address="$2"

	local packet_count=$(tcpdump_show "$interface" | grep \
			"> ${ip_address}.1234" | wc -l)
	echo "Packet count for ${interface} was ${packet_count}"

	if [[ "${packet_count}" -gt 0 ]]; then
		true
	else
		false
	fi
}
