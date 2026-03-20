#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e
source ppp_common.sh

# Temporary files for PTY symlinks
TTY_SERVER=$(mktemp -u /tmp/ppp_async_server.XXXXXX)
TTY_CLIENT=$(mktemp -u /tmp/ppp_async_client.XXXXXX)

cleanup() {
	cleanup_all_ns
	[ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null || true
}

trap cleanup EXIT

require_command socat
ppp_common_init

# Create the virtual serial device
socat -d PTY,link="$TTY_SERVER",rawer PTY,link="$TTY_CLIENT",rawer &
SOCAT_PID=$!

# Wait for symlinks to be created
slowwait 5 [ -L "$TTY_SERVER" ]

# Start the PPP Server
ip netns exec "$NS_SERVER" pppd "$TTY_SERVER" 115200 \
	"$IP_SERVER":"$IP_CLIENT" \
	local noauth nodefaultroute debug

# Start the PPP Client
ip netns exec "$NS_CLIENT" pppd "$TTY_CLIENT" 115200 \
	local noauth updetach nodefaultroute debug

ppp_test_connectivity
