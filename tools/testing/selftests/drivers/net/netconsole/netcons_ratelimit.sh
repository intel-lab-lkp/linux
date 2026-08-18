#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# This test exercises the per-target rate limit. It configures a small burst
# over an interval long enough that the bucket is never refilled, sends many
# more messages than the burst allows, and checks that the target stops
# transmitting once the bucket is empty.
#
# Clearing the interval has to restore unlimited delivery and tell the
# receiver how many messages it missed, which is verified last.
#
# Author: Breno Leitao <leitao@debian.org>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/../lib/sh/lib_netcons.sh

# Messages sent while the limit is in place, comfortably above BURST so that
# the bucket is drained
MSG_COUNT=50
BURST=5
# Long enough that the bucket is not refilled while the test runs
INTERVAL_MS=60000
# Default the target starts with, as documented in netconsole.rst
DEFAULT_BURST=10
# What the target sends once it can transmit again
DROP_NOTICE="messages dropped by rate limit"

# The content of kmsg will be saved to the following file
OUTPUT_FILE="/tmp/${TARGET}"

function count_msgs() {
	local FILE="${1}"

	if [ ! -f "${FILE}" ]
	then
		echo 0
		return
	fi

	# grep exits 1 on no match, which is a valid result here
	grep -c "${MSG}" "${FILE}" || true
}

function send_msgs() {
	local COUNT="${1}"
	local I

	for I in $(seq "${COUNT}")
	do
		echo "${MSG}: ${TARGET} ${I}" > /dev/kmsg
	done
}

# A freshly created target has to be unlimited, otherwise every existing
# netconsole user would start dropping messages after an upgrade
function check_defaults() {
	local INTERVAL BURST_DEFAULT

	INTERVAL=$(cat "${NETCONS_PATH}"/ratelimit_interval_ms)
	BURST_DEFAULT=$(cat "${NETCONS_PATH}"/ratelimit_burst)

	if [ "${INTERVAL}" -ne 0 ] ||
	   [ "${BURST_DEFAULT}" -ne "${DEFAULT_BURST}" ]
	then
		echo "FAIL: unexpected rate limit defaults:" \
		     "interval=${INTERVAL} burst=${BURST_DEFAULT}" >&2
		exit "${ksft_fail}"
	fi
}

function check_limited() {
	local RECEIVED

	RECEIVED=$(count_msgs "${OUTPUT_FILE}")

	# Unrelated kernel messages share the bucket, so fewer than BURST of
	# ours can get through, but never more
	if [ "${RECEIVED}" -gt "${BURST}" ]
	then
		echo "FAIL: received ${RECEIVED} messages with ratelimit_burst=${BURST}" >&2
		cat "${OUTPUT_FILE}" >&2
		exit "${ksft_fail}"
	fi
}

# The notice below travels ahead of the message that reopened the bucket, so
# waiting for the file to appear is not enough
function msg_received() {
	grep -q "${MSG}" "${OUTPUT_FILE}" 2> /dev/null
}

# The messages lost above have to be reported to the receiver
function check_drops_reported() {
	if ! grep -q "${DROP_NOTICE}" "${OUTPUT_FILE}"
	then
		echo "FAIL: no rate limit notice in ${OUTPUT_FILE}" >&2
		cat "${OUTPUT_FILE}" >&2
		exit "${ksft_fail}"
	fi
}

# ========== #
# Start here #
# ========== #

modprobe netdevsim 2> /dev/null || true
modprobe netconsole 2> /dev/null || true

# Check for basic system dependency and exit if not found
check_for_dependencies
# Remove the namespace, interfaces and netconsole target on exit
trap cleanup EXIT

# Set current loglevel to KERN_INFO(6), and default to KERN_NOTICE(5)
echo "6 5" > /proc/sys/kernel/printk
# Create one namespace and two interfaces
set_network
# Create a dynamic target for netconsole
create_dynamic_target

check_defaults

# Set the burst before the interval, so that no message escapes while the
# target still carries the default burst
echo "${BURST}" > "${NETCONS_PATH}"/ratelimit_burst
echo "${INTERVAL_MS}" > "${NETCONS_PATH}"/ratelimit_interval_ms

listen_port_and_save_to "${OUTPUT_FILE}" &
wait_for_port "${NAMESPACE}" "${PORT}" "ipv4"
send_msgs "${MSG_COUNT}"
# This half of the test is about messages that never arrive, so there is
# nothing to busywait on
sleep 1
pkill_socat
check_limited
rm -f "${OUTPUT_FILE}"

# Dropping the interval back to zero has to make the target unlimited again
echo 0 > "${NETCONS_PATH}"/ratelimit_interval_ms

listen_port_and_save_to "${OUTPUT_FILE}" &
wait_for_port "${NAMESPACE}" "${PORT}" "ipv4"
send_msgs 1
if ! busywait "${BUSYWAIT_TIMEOUT}" msg_received
then
	echo "FAIL: Timed out waiting (${BUSYWAIT_TIMEOUT} ms) for netconsole" \
	     "message in ${OUTPUT_FILE} after clearing the rate limit" >&2
	exit "${ksft_fail}"
fi
validate_msg "${OUTPUT_FILE}"
check_drops_reported
pkill_socat
rm -f "${OUTPUT_FILE}"

trap - EXIT
cleanup
exit "${ksft_pass}"
