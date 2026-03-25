#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

source ppp_common.sh

VETH_SERVER="veth-server"
VETH_CLIENT="veth-client"

cleanup() {
	cleanup_all_ns
}

trap cleanup EXIT

require_command pppoe-server
ppp_common_init
modprobe -q pppoe

# Create the veth pair
ip link add "$VETH_SERVER" type veth peer name "$VETH_CLIENT"
ip link set "$VETH_SERVER" netns "$NS_SERVER"
ip link set "$VETH_CLIENT" netns "$NS_CLIENT"
ip -netns "$NS_SERVER" link set "$VETH_SERVER" up
ip -netns "$NS_CLIENT" link set "$VETH_CLIENT" up

# Start the PPP Server
ip netns exec "$NS_SERVER" pppoe-server -I "$VETH_SERVER" \
	-L "$IP_SERVER" -R "$IP_CLIENT" -N 1 -q "$(command -v pppd)" \
	-k -O "$(pwd)/pppoe-server-options"

# Start the PPP Client
ip netns exec "$NS_CLIENT" pppd \
	local debug updetach noipdefault noauth nodefaultroute \
	plugin pppoe.so nic-"$VETH_CLIENT"

ppp_test_connectivity

log_test "PPPoE"

exit $EXIT_STATUS
