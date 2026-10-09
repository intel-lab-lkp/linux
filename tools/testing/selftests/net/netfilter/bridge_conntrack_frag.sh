#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Proxmox Server Solutions GmbH, Gabriel Goller <g.goller@proxmox.com>
#
# Bridge conntrack reassembles fragments. Packets passed up a bridge must be
# refragmented, or a stacked bridge drops them if they exceed its egress MTU:
#
# vlan:    sender                   bridge                       receiver
#          eth0.241 -- eth0 <-> veth0 -- br0 -- br0.241 -- br1 -- veth1 <-> eth0
#
# macvlan: sender             bridge                             receiver
#          eth0 <-> veth0 -- br0 -- mv0 (passthru) -- br1 -- veth1 <-> eth0
#
# br0 is promiscuous because its upper device is a bridge port.

source lib.sh

checktool "nft --version" "run test without nft"

ret=0

trap cleanup_all_ns EXIT

add_dev()
{
	if ! ip "$@"; then
		# Do not hide failures of an earlier topology.
		[ "$ret" -eq 0 ] || exit "$ret"
		echo "SKIP: Cannot create required network device"
		exit $ksft_skip
	fi
}

setup_topology()
{
	local upper=$1 knob

	setup_ns sender nsbr receiver || exit $ksft_skip

	add_dev link add veth0 netns "$nsbr" type veth peer name eth0 netns "$sender"
	add_dev link add veth1 netns "$nsbr" type veth peer name eth0 netns "$receiver"
	add_dev -net "$nsbr" link add br0 type bridge
	add_dev -net "$nsbr" link add br1 type bridge

	case "$upper" in
	vlan)
		add_dev -net "$nsbr" link add link br0 name br0.241 type vlan id 241
		add_dev -net "$sender" link add link eth0 name eth0.241 type vlan id 241
		upper_dev=br0.241
		sender_dev=eth0.241
		;;
	macvlan)
		add_dev -net "$nsbr" link add link br0 name mv0 type macvlan mode passthru
		upper_dev=mv0
		sender_dev=eth0
		;;
	esac

	# br1 could otherwise take over br0's address from its upper device port,
	# and traffic to br1 would no longer reach br0 via promiscuous delivery.
	ip -net "$nsbr" link set br1 address 02:00:00:00:00:20

	# br_netfilter would call the inet hooks on bridged traffic.
	for knob in bridge-nf-call-iptables bridge-nf-call-ip6tables; do
		ip netns exec "$nsbr" sysctl -qw "net.bridge.$knob=0" 2>/dev/null
	done

	ip -net "$nsbr" link set veth0 master br0
	ip -net "$nsbr" link set "$upper_dev" master br1
	ip -net "$nsbr" link set veth1 master br1

	for dev in veth0 veth1 br0 "$upper_dev" br1; do
		ip -net "$nsbr" link set "$dev" up
	done
	ip -net "$sender" link set eth0 up
	ip -net "$sender" link set "$sender_dev" up
	ip -net "$receiver" link set eth0 up

	ip -net "$sender" addr add 192.0.2.1/24 dev "$sender_dev"
	ip -net "$receiver" addr add 192.0.2.2/24 dev eth0
	ip -net "$nsbr" addr add 192.0.2.3/24 dev br1
	ip -net "$sender" addr add 2001:db8:1::1/64 dev "$sender_dev" nodad
	ip -net "$receiver" addr add 2001:db8:1::2/64 dev eth0 nodad
	ip -net "$nsbr" addr add 2001:db8:1::3/64 dev br1 nodad
}

check_ping()
{
	local ns=$1 dst=$2 desc=$3

	if ip netns exec "$ns" ping -q -c 2 -i 0.1 -w 3 -M dont -s 4000 "$dst" \
		>/dev/null 2>&1; then
		echo "PASS: $phase $desc"
	else
		echo "FAIL: $phase $desc"
		ret=1
	fi
}

check_paths()
{
	local prefix

	for prefix in 192.0.2. 2001:db8:1::; do
		check_ping "$sender" "${prefix}2" "sender -> receiver ${prefix}2"
		check_ping "$receiver" "${prefix}1" "receiver -> sender ${prefix}1"
		check_ping "$sender" "${prefix}3" "sender -> bridge host ${prefix}3"
	done
}

run_test()
{
	local upper=$1 family

	setup_topology "$upper"

	phase="$upper without conntrack:"
	check_paths
	# A broken setup must not be hidden by a later skip.
	[ "$ret" -eq 0 ] || exit "$ret"

	# The ct expression enables the bridge conntrack hooks. Count oversized
	# packets at br0 input to make sure they were reassembled there.
	if ! ip netns exec "$nsbr" nft -f - <<'NFT'
table bridge br_frag {
	counter tracked4 { }
	counter tracked6 { }
	chain input {
		type filter hook input priority 0; policy accept;
		iifname "veth0" ct state new,established meta length > 1500 \
			ip saddr 192.0.2.1 counter name tracked4
		iifname "veth0" ct state new,established meta length > 1500 \
			ip6 saddr 2001:db8:1::1 counter name tracked6
	}
}
NFT
	then
		echo "SKIP: Bridge conntrack is not available"
		exit $ksft_skip
	fi

	phase="$upper with conntrack:"
	check_paths

	for family in 4 6; do
		if ! ip netns exec "$nsbr" nft list counter bridge br_frag "tracked$family" |
			grep -q 'packets [1-9]'; then
			echo "FAIL: $phase no reassembled IPv$family packets at br0 input"
			ret=1
		fi
	done
}

run_test vlan
run_test macvlan

exit "$ret"
