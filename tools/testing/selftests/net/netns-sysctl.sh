#!/bin/bash -e
# SPDX-License-Identifier: GPL-2.0
#
# This test checks that the network buffer sysctls are present
# in a network namespaces, and that they are readonly.

source lib.sh

cleanup() {
    cleanup_ns $test_ns
}

trap cleanup EXIT

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

setup_ns test_ns

for sc in {r,w}mem_{default,max}; do
	initial_value="$(sysctl -n "net.core.$sc")"

	# check that this is writable in the init netns
	[ -w "/proc/sys/net/core/$sc" ] ||
		fail "$sc isn't writable in the init netns!"

	# change the value in the init netns
	sysctl -qw "net.core.$sc=300000" ||
		fail "Can't write $sc in init netns!"

	# check that the value did not change in the test netns
	[ "$(ip netns exec $test_ns sysctl -n "net.core.$sc")" -eq "$initial_value" ] ||
		fail "Value for $sc mismatch!"

	# check that this is also writable in the test netns
	ip netns exec $test_ns [ -w "/proc/sys/net/core/$sc" ] ||
		fail "$sc isn't writable in the test netns!"

	# change the value in the test netns
	ip netns exec $test_ns sysctl -qw "net.core.$sc=200000" ||
		fail "Can't write $sc in test netns!"

	# check that the value is read from the test netns
	[ "$(ip netns exec $test_ns sysctl -n "net.core.$sc")" -eq 200000 ] ||
		fail "Value for $sc mismatch!"
done

echo 'Test passed OK'
