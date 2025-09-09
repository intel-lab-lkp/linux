#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# This test validates that netconsole is able to resume a target that was
# deactivated when its interface was removed.
#
# The test configures a netconsole dynamic target and then removes netdevsim
# module to cause the interface to disappear. The test veries that the target
# moved to disabled state before adding netdevsim and the interface back.
#
# Finally, the test verifies that the target is re-enabled automatically and
# the message is received on the destination interface.
#
# Author: Andre Carvalho <asantostc@gmail.com>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/lib/sh/lib_netcons.sh

modprobe netdevsim 2> /dev/null || true
modprobe netconsole 2> /dev/null || true

# The content of kmsg will be save to the following file
OUTPUT_FILE="/tmp/${TARGET}"

# Check for basic system dependency and exit if not found
check_for_dependencies

# Set current loglevel to KERN_INFO(6), and default to KERN_NOTICE(5)
echo "6 5" > /proc/sys/kernel/printk
# Remove the namespace, interfaces and netconsole target on exit
trap cleanup EXIT

# Create one namespace and two interfaces
set_network
# Create a dynamic target for netconsole
create_dynamic_target

# Remove low level module
rmmod netdevsim
# Target should be disabled
wait_target_state "${TARGET}" "disabled"

# Add back low level module
modprobe netdevsim
# Recreate namespace and two interfaces
set_network
# Target should be enabled again
wait_target_state "${TARGET}" "enabled"

# Listed for netconsole port inside the namespace and destination
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

exit "${ksft_pass}"
