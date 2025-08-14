#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This test ensures directed broadcast routes use dst hint mechanism

CLIENT_NS=$(mktemp -u client-XXXXXXXX)
CLIENT_IP4="192.168.0.1"

SERVER_NS=$(mktemp -u server-XXXXXXXX)
SERVER_IP4="192.168.0.2"

BROADCAST_ADDRESS="192.168.0.255"

setup() {
	ip netns add "${CLIENT_NS}"
	ip netns add "${SERVER_NS}"

	ip -net "${SERVER_NS}" link add link1 type veth peer name link0 netns "${CLIENT_NS}"

	ip -net "${CLIENT_NS}" link set link0 up
	ip -net "${CLIENT_NS}" addr add "${CLIENT_IP4}/24" dev link0

	ip -net "${SERVER_NS}" link set link1 up
	ip -net "${SERVER_NS}" addr add "${SERVER_IP4}/24" dev link1

	ip netns exec "${CLIENT_NS}" ethtool -K link0 tcp-segmentation-offload off
	ip netns exec "${SERVER_NS}" sh -c "echo 500000000 > /sys/class/net/link1/gro_flush_timeout"
	ip netns exec "${SERVER_NS}" sh -c "echo 1 > /sys/class/net/link1/napi_defer_hard_irqs"
	ip netns exec "${SERVER_NS}" ethtool -K link1 generic-receive-offload on
}

cleanup() {
	ip -net "${SERVER_NS}" link del link1
	ip netns del "${CLIENT_NS}"
	ip netns del "${SERVER_NS}"
}

directed_bcast_hint_test()
{
	echo "Testing for directed broadcast route hint"

	orig_in_brd=$(ip netns exec "${SERVER_NS}" lnstat -k in_brd -s0 -i1 -c1 | tr -d ' |')
	ip netns exec "${CLIENT_NS}" mausezahn link0 -a own -b bcast -A "${CLIENT_IP4}" \
		-B "${BROADCAST_ADDRESS}" -c1 -t tcp "sp=1-100,dp=1234,s=1,a=0" -p 5 -q
	sleep 1
	new_in_brd=$(ip netns exec "${SERVER_NS}" lnstat -k in_brd -s0 -i1 -c1 | tr -d ' |')

	res=$(echo "${new_in_brd} - ${orig_in_brd}" | bc)

	[ "${res}" -lt 100 ]
}

trap cleanup EXIT

setup

directed_bcast_hint_test
exit $?
