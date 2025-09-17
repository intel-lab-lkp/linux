#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# This test verifies netconsole functionality over a bonded network setup.
#
# Four interfaces are created using netdevsim; two of them are bonded to serve
# as the netconsole's transmit interface. The remaining two interfaces are
# similarly bonded and assigned to a separate network namespace, which acts as
# the receive interface, where socat monitors for incoming messages.
#
# A netconsole message is then sent to ensure it is properly received across
# this configuration.
#
# The test's objective is to exercise netpoll usage when managed simultaneously
# by multiple subsystems (netconsole and bonding).
#
# Author: Breno Leitao <leitao@debian.org>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/lib/sh/lib_netcons.sh

modprobe netdevsim 2> /dev/null || true
modprobe netconsole 2> /dev/null || true
modprobe bonding 2> /dev/null || true

# The content of kmsg will be save to the following file
OUTPUT_FILE="/tmp/${TARGET}"

# Check for basic system dependency and exit if not found
check_for_dependencies
# Set current loglevel to KERN_INFO(6), and default to KERN_NOTICE(5)
echo "6 5" > /proc/sys/kernel/printk
# Remove the namespace, interfaces and netconsole target on exit
trap cleanup_bond EXIT

# Run the test twice, with different format modes
for FORMAT in "basic" "extended"
do
	for IP_VERSION in "ipv6" "ipv4"
	do
		# Run the test twice, with different format modes
		echo "Running netcons over bonding ifaces: ${FORMAT} (${IP_VERSION})"

		# Create one namespace and two interfaces
		set_network "${IP_VERSION}" "bonding"
		# Create a dynamic target for netconsole
		create_dynamic_target "${FORMAT}"
		# Only set userdata for extended format
		if [ "$FORMAT" == "extended" ]
		then
			# Set userdata "key" with the "value" value
			set_user_data
		fi
		# Listed for netconsole port inside the namespace and
		# destination interface
		listen_port_and_save_to "${OUTPUT_FILE}" "${IP_VERSION}" &
		# Wait for socat to start and listen to the port.
		wait_for_port "${NAMESPACE}" "${PORT}" "${IP_VERSION}"
		# Send the message
		echo "${MSG}: ${TARGET}" > /dev/kmsg
		# Wait until socat saves the file to disk
		busywait "${BUSYWAIT_TIMEOUT}" test -s "${OUTPUT_FILE}"
		# Make sure the message was received in the dst part
		# and exit
		validate_result "${OUTPUT_FILE}" "${FORMAT}"
		# kill socat in case it is still running
		pkill_socat
		cleanup_bond
		echo "bonding: ${FORMAT} : ${IP_VERSION} : Test passed" >&2
	done
done

trap - EXIT
exit "${EXIT_STATUS}"
