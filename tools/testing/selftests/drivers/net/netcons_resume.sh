#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# This test validates that netconsole is able to resume a target that was
# deactivated when its interface was removed when the interface is brought
# back up.
#
# The test configures a netconsole target and then removes netdevsim module to
# cause the interface to disappear. Targets are configured via cmdline to ensure
# targets bound by interface name and mac address can be resumed.
# The test verifies that the target moved to disabled state before adding
# netdevsim and the interface back.
#
# Finally, the test verifies that the target is re-enabled automatically and
# the message is received on the destination interface.
#
# Author: Andre Carvalho <asantostc@gmail.com>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/lib/sh/lib_netcons.sh

modprobe netdevsim 2> /dev/null || true
rmmod netconsole 2> /dev/null || true

check_netconsole_module

# Run the test twice, with different cmdline parameters
for BINDMODE in "ifname" "mac"
do
	echo "Running with bind mode: ${BINDMODE}" >&2
	# Set current loglevel to KERN_INFO(6), and default to KERN_NOTICE(5)
	echo "6 5" > /proc/sys/kernel/printk

	# Create one namespace and two interfaces
	set_network
	trap do_cleanup EXIT

	# Create the command line for netconsole, with the configuration from
	# the function above
	CMDLINE=$(create_cmdline_str "${BINDMODE}")

	# The content of kmsg will be save to the following file
	OUTPUT_FILE="/tmp/${TARGET}-${BINDMODE}"

	# Load the module, with the cmdline set
	modprobe netconsole "${CMDLINE}"
	# Expose cmdline target in configfs
	mkdir ${NETCONS_CONFIGFS}"/cmdline0"
	trap 'cleanup "${NETCONS_CONFIGFS}"/cmdline0' EXIT

	# Target should be enabled
	wait_target_state "cmdline0" "enabled"

	# Remove low level module
	rmmod netdevsim
	# Target should be disabled
	wait_target_state "cmdline0" "disabled"

	# Add back low level module
	modprobe netdevsim
	# Recreate namespace and two interfaces
	set_network
	# Target should be enabled again
	wait_target_state "cmdline0" "enabled"

	# Listen for netconsole port inside the namespace and destination
	# interface
	listen_port_and_save_to "${OUTPUT_FILE}" &
	# Wait for socat to start and listen to the port.
	wait_local_port_listen "${NAMESPACE}" "${PORT}" udp
	# Send the message
	echo "${MSG}: ${TARGET}" > /dev/kmsg
	# Wait until socat saves the file to disk
	busywait "${BUSYWAIT_TIMEOUT}" test -s "${OUTPUT_FILE}"
	# Make sure the message was received in the dst part
	# and exit
	validate_msg "${OUTPUT_FILE}"

	# kill socat in case it is still running
	pkill_socat
	# Cleanup & unload the module
	cleanup "${NETCONS_CONFIGFS}/cmdline0"
	rmmod netconsole
	trap - EXIT

	echo "${BINDMODE} : Test passed" >&2
done

exit "${ksft_pass}"
