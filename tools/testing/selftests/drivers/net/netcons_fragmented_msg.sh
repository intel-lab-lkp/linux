#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# Test netconsole's message fragmentation functionality.
#
# When a message exceeds the maximum packet size, netconsole splits it into
# multiple fragments for transmission. This test verifies:
#  - Correct fragmentation of large messages
#  - Proper reassembly of fragments at the receiver
#  - Preservation of userdata across fragments
#  - Behavior with and without kernel release version appending
#
# Author: Breno Leitao <leitao@debian.org>

set -euo pipefail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "${SCRIPTDIR}"/lib/sh/lib_netcons.sh

modprobe netdevsim 2> /dev/null || true
modprobe netconsole 2> /dev/null || true

# The content of kmsg will be save to the following file
OUTPUT_FILE="/tmp/${TARGET}"

# set userdata to a long value. In this case, it is "1-2-3-4...50-"
USERDATA_VALUE=$(printf -- '%.2s-' {1..60})

# Convert the header string in a regexp, so, we can remove
# the second header as well.
# A header looks like "13,468,514729715,-,ncfrag=0/1135;". If
# release is appended, you might find something like:L
# "6.13.0-04048-g4f561a87745a,13,468,514729715,-,ncfrag=0/1135;"
function header_to_regex() {
	# header is everything before ;
	local HEADER="${1}"
	REGEX=$(echo "${HEADER}" | cut -d'=' -f1)
	echo "${REGEX}=[0-9]*\/[0-9]*;"
}

# We have two headers in the message. Remove both to get the full message,
# and extract the full message.
function extract_msg() {
	local MSGFILE="${1}"
	# Extract the header, which is the very first thing that arrives in the
	# first list.
	HEADER=$(sed -n '1p' "${MSGFILE}" | cut -d';' -f1)
	HEADER_REGEX=$(header_to_regex "${HEADER}")

	# Remove the two headers from the received message
	# This will return the message without any header, similarly to what
	# was sent.
	sed "s/""${HEADER_REGEX}""//g" "${MSGFILE}"
}

# Validate the message, which has two messages glued together.
# unwrap them to make sure all the characters were transmitted.
# File will look like the following:
#   13,468,514729715,-,ncfrag=0/1135;MSG1=MSG2=MSG3=MSG4=MSG5=MSG6=MSG7=MSG8=MSG9=MSG10=MSG11=MSG12=MSG13=MSG14=MSG15=MSG16=MSG17=MSG18=MSG19=MSG20=MSG21=MSG22=MSG23=MSG24=MSG25=MSG26=MSG27=MSG28=MSG29=MSG30=MSG31=MSG32=MSG33=MSG34=MSG35=MSG36=MSG37=MSG38=MSG39=MSG40=MSG41=MSG42=MSG43=MSG44=MSG45=MSG46=MSG47=MSG48=MSG49=MSG50=MSG51=MSG52=MSG53=MSG54=MSG55=MSG56=MSG57=MSG58=MSG59=MSG60=MSG61=MSG62=MSG63=MSG64=MSG65=MSG66=MSG67=MSG68=MSG69=MSG70=MSG71=MSG72=MSG73=MSG74=MSG75=MSG76=MSG77=MSG78=MSG79=MSG80=MSG81=MSG82=MSG83=MSG84=MSG85=MSG86=MSG87=MSG88=MSG89=MSG90=MSG91=MSG92=MSG93=MSG94=MSG95=MSG96=MSG97=MSG98=MSG99=MSG100=MSG101=MSG102=MSG103=MSG104=MSG105=MSG106=MSG107=MSG108=MSG109=MSG110=MSG111=MSG112=MSG113=MSG114=MSG115=MSG116=MSG117=MSG118=MSG119=MSG120=MSG121=MSG122=MSG123=MSG124=MSG125=MSG126=MSG127=MSG128=MSG129=MSG130=MSG131=MSG132=MSG133=MSG134=MSG135=MSG136=MSG137=MSG138=MSG139=MSG140=MSG141=MSG142=MSG143=MSG144=MSG145=MSG146=MSG147=MSG148=MSG149=MSG150=: netcons_nzmJQ
#    key=1-2-13,468,514729715,-,ncfrag=967/1135;3-4-5-6-7-8-9-10-11-12-13-14-15-16-17-18-19-20-21-22-23-24-25-26-27-28-29-30-31-32-33-34-35-36-37-38-39-40-41-42-43-44-45-46-47-48-49-50-51-52-53-54-55-56-57-58-59-60-
function validate_fragmented_result() {
	# Discard the netconsole headers, and assemble the full message
	RCVMSG=$(extract_msg "${1}")

	# check for the main message
	if ! echo "${RCVMSG}" | grep -q "${MSG}"; then
		echo "Message body doesn't match." >&2
		echo "msg received=" "${RCVMSG}" >&2
		exit "${ksft_fail}"
	fi

	# check userdata
	if ! echo "${RCVMSG}" | grep -q "${USERDATA_VALUE}"; then
		echo "message userdata doesn't match" >&2
		echo "msg received=" "${RCVMSG}" >&2
		exit "${ksft_fail}"
	fi
	# test passed. hooray
}

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
# Set userdata "key" with the "value" value
set_user_data


# TEST 1: Send message and userdata. They will fragment
# =======
MSG=$(printf -- 'MSG%.3s=' {1..150})

# Listen for netconsole port inside the namespace and destination interface
listen_port_and_save_to "${OUTPUT_FILE}" &
# Wait for socat to start and listen to the port.
wait_local_port_listen "${NAMESPACE}" "${PORT}" udp
# Send the message
echo "${MSG}: ${TARGET}" > /dev/kmsg
# Wait until socat saves the file to disk
busywait "${BUSYWAIT_TIMEOUT}" test -s "${OUTPUT_FILE}"
# Check if the message was not corrupted
validate_fragmented_result "${OUTPUT_FILE}"

# TEST 2: Test with smaller message, and without release appended
# =======
MSG=$(printf -- 'FOOBAR%.3s=' {1..100})
# Let's disable release and test again.
disable_release_append

listen_port_and_save_to "${OUTPUT_FILE}" &
wait_local_port_listen "${NAMESPACE}" "${PORT}" udp
echo "${MSG}: ${TARGET}" > /dev/kmsg
busywait "${BUSYWAIT_TIMEOUT}" test -s "${OUTPUT_FILE}"
validate_fragmented_result "${OUTPUT_FILE}"
exit "${ksft_pass}"
