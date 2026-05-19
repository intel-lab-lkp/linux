#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2154
#
# Exercise nft_fib6_eval()'s sibling/nh enumeration on three route shapes:
#   1) route via a single external nexthop (nhid)
#   2) route via an external nexthop group (nhid -> group)
#   3) route via old-style multipath (nexthop ... nexthop ...)
#
# Topology similar to nft_fib.sh, without ns2; two dummy interfaces on
# nsrouter host the nh devices:
#
#   dead:1::99           dead:1::1     dummy0 dead:2::1
#   ns1 <-----veth----->         nsrouter
#                                       dummy1 dead:9::1

source lib.sh

ret=0

checktool "nft --version" "run test without nft"
checktool "ip -V"         "run test without iproute2"

setup_ns nsrouter ns1
trap cleanup_all_ns EXIT

if ! ip link add veth0 netns "$nsrouter" type veth peer name eth0 netns "$ns1" \
	> /dev/null 2>&1; then
	echo "SKIP: No virtual ethernet pair device support in kernel"
	exit $ksft_skip
fi

ip -net "$ns1" link set lo up
ip -net "$ns1" link set eth0 up
ip -net "$ns1" -6 addr add dead:1::99/64 dev eth0 nodad
ip -net "$ns1" -6 route add default via dead:1::1

ip -net "$nsrouter" link set lo up
ip -net "$nsrouter" link set veth0 up
ip -net "$nsrouter" -6 addr add dead:1::1/64 dev veth0 nodad

if ! ip -net "$nsrouter" link add dummy0 type dummy 2>/dev/null; then
	echo "SKIP: dummy netdev not available"
	exit $ksft_skip
fi
ip -net "$nsrouter" link set dummy0 up
ip -net "$nsrouter" -6 addr add dead:2::1/64 dev dummy0 nodad

ip -net "$nsrouter" link add dummy1 type dummy
ip -net "$nsrouter" link set dummy1 up
ip -net "$nsrouter" -6 addr add dead:9::1/64 dev dummy1 nodad

ip netns exec "$nsrouter" sysctl -q net.ipv6.conf.all.forwarding=1

load_fib_rule() {
	ip netns exec "$nsrouter" nft -f /dev/stdin <<EOF
flush ruleset
table ip6 t {
	chain c {
		type filter hook prerouting priority 0; policy accept;
		fib daddr . iif oif missing counter
	}
}
EOF
}

run_scenario() {
	local what="$1"; shift

	ip -net "$nsrouter" -6 route del dead:dead::/64 2>/dev/null || true
	ip -net "$nsrouter" -6 nexthop flush 2>/dev/null || true

	"$@" || { echo "SKIP ($what): could not configure route"; return; }

	load_fib_rule || { echo "FAIL ($what): nft load"; ret=1; return; }

	ip netns exec "$ns1" ping -6 -c 1 -W 1 dead:dead::1 \
		> /dev/null 2>&1 || true

	echo "PASS: $what"
}

scenario_single_nh() {
	ip -net "$nsrouter" nexthop add id 1 via dead:2::2 dev dummy0
	ip -net "$nsrouter" -6 route add dead:dead::/64 nhid 1
}
run_scenario "single external nexthop (nhid)" scenario_single_nh

scenario_nh_group() {
	ip -net "$nsrouter" nexthop add id 1   via dead:2::2 dev dummy0
	ip -net "$nsrouter" nexthop add id 2   via dead:9::2 dev dummy1
	ip -net "$nsrouter" nexthop add id 100 group 1/2
	ip -net "$nsrouter" -6 route   add dead:dead::/64 nhid 100
}
run_scenario "nexthop group (nhid group 1/2)" scenario_nh_group

scenario_old_multipath() {
	ip -net "$nsrouter" -6 route add dead:dead::/64 \
		nexthop via dead:2::2 dev dummy0 \
		nexthop via dead:9::2 dev dummy1
}
run_scenario "old-style multipath (fib6_siblings)" scenario_old_multipath

exit $ret
