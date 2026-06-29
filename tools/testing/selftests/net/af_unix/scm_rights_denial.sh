#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

#
# scm_rights_denial.sh - Test SCM_RIGHTS fd passing using Smack LSM blocking
#
# Must be run as root on a kernel with Smack enabled (security=smack).
# Requires: capsh (libcap), setfattr/getfattr (attr)
#
# We use the following Smack labels:
#   "Sender"   - label for the sending process
#   "Receiver" - label for the receiving process
#   "SecretX"   - labels for the files being passed
#
# Socket communication (Sender <-> Receiver) is always allowed.
# The tests control whether Receiver can access "SecretX"-labeled fds.
#

set -e

readonly KSFT_SKIP=4

readonly SENDER="./scm_rights_denial_sender"
readonly RECEIVER="./scm_rights_denial_receiver"

readonly TESTDIR="$(mktemp -d)"
readonly SOCK="$TESTDIR/scm_test.sock"
readonly TESTFILE1="$TESTDIR/secret_1"
readonly TESTFILE2="$TESTDIR/secret_2"

trap 'rm -rf "$TESTDIR"' EXIT

run_tests() {

	preflight
	setup

	run_test "TEST 1" \
		"Receiver should NOT have access to Secret1." \
		"Receiver Secret1 ---
Receiver Secret2 ---" \
		"$TESTFILE1" \
		"BLOCKED"

	run_test "TEST 2" \
		"Receiver should have access to Secret1." \
		"Receiver Secret1 r--
Receiver Secret2 ---" \
		"$TESTFILE1" \
		"PASSED"

	run_test "TEST 3" \
		"Receiver should have access to Secret2, but NOT Secret1." \
		"Receiver Secret1 ---
Receiver Secret2 r--" \
		"$TESTFILE1 $TESTFILE2" \
		"BLOCKED PASSED"
}

run_test() {
	local name="$1"
	local description="$2"
	local rules="$3"
	local files="$4"
	local expected="$5"

	echo ""
	echo "$name: $description"
	echo "Rules:"
	echo "$rules"
	echo "Expected: $expected"
	echo ""

	while IFS= read -r rule; do
		[ -n "$rule" ] && echo "$rule" > /sys/fs/smackfs/load2
	done <<< "$rules"

	local output status last_line
	output=$(send_fds "$SOCK" $files)
	status=$?
	echo "$output"
	last_line=$(echo "$output" | tail -n 1 | xargs)

	if [ "$status" -ne 0 ]; then
		echo "TEST FAILED: receiver returned $status"
		return 1
	fi

	if [[ "$last_line" == "$expected" ]]; then
		echo "TEST PASSED: outcome was $expected as expected"
		return 0
	else
		echo "TEST FAILED: expected $expected, got '$last_line'"
		return 1
	fi
}

setup() {

	printf "Secret 1" > "$TESTFILE1"
	printf "Secret 2" > "$TESTFILE2"

	setfattr -n security.SMACK64 -v "Secret1" "$TESTFILE1"
	setfattr -n security.SMACK64 -v "Secret2" "$TESTFILE2"
	setfattr -n security.SMACK64 -v "Tmp" /tmp
	setfattr -n security.SMACK64 -v "Tmp" "$TESTDIR"

	echo "Sender	Receiver	-w-" > /sys/fs/smackfs/load2
	echo "Receiver	Sender		-w-" > /sys/fs/smackfs/load2
	echo "Sender	Tmp 		rwx" > /sys/fs/smackfs/load2
	echo "Receiver	Tmp		rwx" > /sys/fs/smackfs/load2
	echo "Sender	Secret1		r--" > /sys/fs/smackfs/load2
	echo "Sender	Secret2		r--" > /sys/fs/smackfs/load2
}

send_fds() {

	local sk="$1"
	shift
	local files="$*"

	(
	    echo "Receiver" > /proc/self/attr/current
	    exec capsh --drop=cap_mac_override,cap_mac_admin -- -c "$RECEIVER $sk"
	) &
	local recv_pid=$!
	sleep 1

	(
	    echo "Sender" > /proc/self/attr/current
	    exec capsh --drop=cap_mac_override,cap_mac_admin -- -c "$SENDER $sk $files"
	) || true

	local recv_status=0
	wait "$recv_pid" || recv_status=$?

	if [ "$recv_status" -ne 0 ]; then
	    echo "receiver exited with $recv_status"
	fi
	return "$recv_status"
}

preflight() {

	if [ "$(id -u)" -ne 0 ]; then
	    echo "SKIP: must be run as root"
	    exit $KSFT_SKIP
	fi

	if ! grep -q smack /sys/kernel/security/lsm 2>/dev/null; then
	    echo "SKIP: Smack is not active"
	    echo "  Check: cat /sys/kernel/security/lsm"
	    echo "  Boot with: security=smack"
	    exit $KSFT_SKIP
	fi

	if ! mountpoint -q /sys/fs/smackfs 2>/dev/null; then
	    echo "Mounting smackfs..."
	    mount -t smackfs smackfs /sys/fs/smackfs
	fi

	if ! command -v capsh &>/dev/null; then
	    echo "SKIP: capsh not found (install libcap)"
	    exit $KSFT_SKIP
	fi

	if [ ! -x "$SENDER" ] || [ ! -x "$RECEIVER" ]; then
	    echo "ERROR: $SENDER / $RECEIVER not built (run 'make' first)"
	    exit 1
	fi

}

run_tests
