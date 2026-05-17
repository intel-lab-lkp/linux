#!/bin/sh -euf
# SPDX-License-Identifier: GPL-2.0
#
# selftests/net/tcp_repair: TCP_REPAIR connection tests
#
# outer.sh - Set up outer namespace, run test server there
#
# Copyright (c) 2026 Red Hat GmbH
#
# Author: Stefano Brivio <sbrivio@redhat.com>

ns_inner_dir="$(mktemp -d)"

cleanup() {
	rm -rf "${ns_inner_dir}"
}

trap cleanup EXIT

# Detach inner namespace in a subshell, tests start from there
unshare -rUn -- ./inner.sh "${ns_inner_dir}" &

# Wait for inner namespace
while [ ! -d "${ns_inner_dir}/pid_ready" ]; do
	sleep 0.1 || sleep 1
done

# Set up link to inner namespace
ip link add veth0 type veth peer name veth0 netns "$(cat "${ns_inner_dir}/pid")"
ip link set dev veth0 up
ip addr add 169.254.2.1 dev veth0
ip ro add default dev veth0

# Run test server
./server 1024

# Wait for test results
while [ ! -d "${ns_inner_dir}/result_ready" ]; do
	sleep 0.1 || sleep 1
done

# Clean up and return results
ret="$(cat "${ns_inner_dir}/result")"
exit "${ret}"
