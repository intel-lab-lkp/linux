#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test that act_nat correctly updates the inner IP header checksum
# when rewriting addresses inside ICMP error payloads.
#
# Setup:
# +---------------------+                      +---------------------+
# | NS1                 |                      | NS2                 |
# |                     |                      |                     |
# |         +-------+   |                      |   +-------+         |
# |         | veth0 +---+----------------------+---+ veth0 |         |
# |         +-------+   |                      |   +-------+         |
# |       10.0.1.1/24   |                      | 10.0.2.1/24         |
# +---------------------+                      +---------------------+
#
# On NS1's veth0:
#   egress act_nat:  src 10.0.1.0/24 -> 10.0.2.0/24
#   ingress act_nat: dst 10.0.2.0/24 -> 10.0.1.0/24
#
# NS1 pings 10.0.2.99 (unreachable in NS2). NS2 sends back an ICMP
# "destination host unreachable". The ICMP error contains a copy of the
# original IP header whose source was NATted. On ingress, act_nat rewrites
# the inner destination back. The inner IP header checksum must be updated.
#
# We use a raw ICMP socket in NS1 to receive the post-NAT ICMP error
# and verify the inner IP header checksum is correct.

source lib.sh

cleanup()
{
	cleanup_ns $NS1 $NS2
}

trap cleanup EXIT

# Check for required modules
for mod in act_nat cls_u32 sch_ingress; do
	modinfo $mod &>/dev/null || { echo "SKIP: Need $mod module"; exit $ksft_skip; }
done

setup_ns NS1 NS2

ip -netns $NS1 link add veth0 type veth peer name veth0 netns $NS2
ip -netns $NS1 link set dev veth0 up
ip -netns $NS2 link set dev veth0 up

ip -netns $NS1 addr add 10.0.1.1/24 dev veth0
ip -netns $NS2 addr add 10.0.2.1/24 dev veth0

ip netns exec $NS2 sysctl -qw net.ipv4.ip_forward=1
ip netns exec $NS2 sysctl -qw net.ipv4.icmp_ratelimit=0

ip -netns $NS1 route add 10.0.2.0/24 dev veth0

# act_nat on NS1's veth0
ip netns exec $NS1 tc qdisc add dev veth0 clsact

# Egress: rewrite src 10.0.1.x -> 10.0.2.x
ip netns exec $NS1 tc filter add dev veth0 egress protocol ip prio 1 \
	u32 match ip src 10.0.1.0/24 \
	action nat egress 10.0.1.0/24 10.0.2.0

# Ingress: rewrite dst 10.0.2.x -> 10.0.1.x
ip netns exec $NS1 tc filter add dev veth0 ingress protocol ip prio 1 \
	u32 match ip dst 10.0.2.0/24 \
	action nat ingress 10.0.2.0/24 10.0.1.0

# Run the test: send ping and capture the ICMP error via raw socket
ip netns exec $NS1 python3 "$(dirname "$0")/act_nat_icmp_csum_verify.py"
exit $?
