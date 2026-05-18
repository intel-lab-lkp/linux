#!/bin/sh -euf
# SPDX-License-Identifier: GPL-2.0
#
# selftests/net/tcp_repair: TCP_REPAIR connection tests
#
# inner.sh - Set up link to outer namespace, run test client in inner namespace
#
# Copyright (c) 2026 Red Hat GmbH
#
# Author: Stefano Brivio <sbrivio@redhat.com>

ns_inner_dir="${1}"

# Tell the parent shell about our PID
echo "${$}" > "${ns_inner_dir}/pid"
mkdir "${ns_inner_dir}/pid_ready"

# Wait for veth to appear
while [ -z "$(sed -n '4p' /proc/net/dev)" ]; do
	sleep 0.1 || sleep 1
done

# Set up link to outer namespace
ip link set dev veth0 up
ip addr add 169.254.2.2 dev veth0
ip ro add default dev veth0

# Finally run tests
set +e
./client 169.254.2.1 1024
echo "${?}" > "${ns_inner_dir}/result"
mkdir "${ns_inner_dir}/result_ready"
