#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# This test verifies that netconsole userdata remains consistent under concurrent
# read/write operations. It creates two loops: one continuously writing netconsole
# messages (which read userdata) and another rapidly alternating userdata values
# between two distinct patterns. The test checks that no message contains corrupted
# or mixed userdata, ensuring proper synchronization in the netconsole implementation.
#
# Author: Gustavo Luiz Duarte <gustavold@gmail.com>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/lib/sh/lib_netcons.sh

function loop_set_userdata() {
	MSGA=$(printf 'A%.0s' {1..198})
	MSGB=$(printf 'B%.0s' {1..198})

	while true; do
		echo "$MSGA" > "${NETCONS_PATH}/userdata/${USERDATA_KEY}/value"
		echo "$MSGB" > "${NETCONS_PATH}/userdata/${USERDATA_KEY}/value"
	done
}

function loop_print_msg() {
	while true; do
		echo "test msg" > /dev/kmsg
	done
}

cleanup_children() {
	pkill_socat
	kill "$child1" "$child2" 2> /dev/null || true
	wait "$child1" "$child2" 2> /dev/null || true
	# Remove the namespace, interfaces and netconsole target
	cleanup
}

modprobe netdevsim 2> /dev/null || true
modprobe netconsole 2> /dev/null || true

OUTPUT_FILE="stdout"
# Check for basic system dependency and exit if not found
check_for_dependencies
# Set current loglevel to KERN_INFO(6), and default to KERN_NOTICE(5)
echo "6 5" > /proc/sys/kernel/printk
# kill child processes and remove interfaces on exit
trap cleanup_children EXIT

# Create one namespace and two interfaces
set_network
# Create a dynamic target for netconsole
create_dynamic_target
# Set userdata "key" with the "value" value
set_user_data

# Start userdata read loop (printk)
loop_print_msg &
child1=$!

# Start userdata write loop
loop_set_userdata &
child2=$!

# Start socat to listen for netconsole messages and check for corrupted userdata.
MAX_COUNT=10000
i=0
while read -r line; do
	if [ $i -ge $MAX_COUNT ]; then
		echo "Test passed."
		exit "${ksft_pass}"
	fi

	if [[ "$line" == "key=A"* && "$line" == *"B"* ||
	      "$line" == "key=B"* && "$line" == *"A"* ]]; then
		echo "Test failed. Found corrupted userdata: $line"
		exit "${ksft_fail}"
	fi

	i=$((i + 1))
done < <(listen_port_and_save_to ${OUTPUT_FILE} 2> /dev/null)

echo "socat died before we could check $MAX_COUNT messages. Skipping test."
exit "${ksft_skip}"
