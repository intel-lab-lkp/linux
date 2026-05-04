#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2034,SC2154
#
# Selftest for the SRv6 End.MAP behavior (RFC 9433 Section 6.2).
#
#   +--------+   2001:db8:1::/64   +--------+   2001:db8:2::/64   +--------+
#   | srupf1 | ------------------- | srupf2 | ------------------- | srupf3 |
#   +--------+       veth-1        +--------+       veth-2        +--------+
#                                (intermediate
#                                 SRv6-aware UPF,
#                                 End.MAP)
#
# All three netns are SRv6-aware UPFs in the RFC 9433 sense (not
# 3GPP UPFs).  Per RFC 9433 Section 6.2 End.MAP is used by the
# intermediate UPF (here srupf2): srupf2 has an End.MAP SID for
# locator 2001:db8:f::/64 mapping to the new SID 2001:db8:2::e.
# srupf1 sends an IPv6 packet to 2001:db8:f::1; on srupf3 the
# destination address is expected to have been replaced by
# 2001:db8:2::e.

source lib.sh

readonly TIMEOUT=4

cleanup()
{
	cleanup_all_ns
}

trap cleanup EXIT

setup()
{
	setup_ns srupf1 srupf2 srupf3

	ip -n "$srupf1" link set lo up
	ip -n "$srupf2" link set lo up
	ip -n "$srupf3" link set lo up

	ip link add veth-1 netns "$srupf1" type veth peer name veth-1-srupf2 \
		netns "$srupf2"
	ip -n "$srupf1" addr add 2001:db8:1::1/64 dev veth-1 nodad
	ip -n "$srupf2" addr add 2001:db8:1::2/64 dev veth-1-srupf2 nodad
	ip -n "$srupf1" link set veth-1 up
	ip -n "$srupf2" link set veth-1-srupf2 up

	ip link add veth-2 netns "$srupf2" type veth peer name veth-2-srupf3 \
		netns "$srupf3"
	ip -n "$srupf2" addr add 2001:db8:2::1/64 dev veth-2 nodad
	ip -n "$srupf3" addr add 2001:db8:2::e/64 dev veth-2-srupf3 nodad
	ip -n "$srupf2" link set veth-2 up
	ip -n "$srupf3" link set veth-2-srupf3 up

	ip netns exec "$srupf2" sysctl -wq net.ipv6.conf.all.forwarding=1

	ip -n "$srupf1" -6 route add 2001:db8:f::/64 via 2001:db8:1::2

	ip -n "$srupf2" -6 route add 2001:db8:f::/64 \
		encap seg6local action End.MAP nh6 2001:db8:2::e \
		dev veth-2

	# allow srupf3 to reply back to srupf1
	ip -n "$srupf3" -6 route add 2001:db8:1::/64 via 2001:db8:2::1
}

check_dependencies()
{
	if ! command -v ping >/dev/null; then
		echo "SKIP: ping is required"; exit "$ksft_skip"
	fi

	if ! ip route help 2>&1 | grep -qF "End.MAP"; then
		echo "SKIP: iproute2 too old, missing seg6local action End.MAP"
		exit "$ksft_skip"
	fi
}

run_test()
{
	# srupf3 replies to ICMPv6 echo on 2001:db8:2::e, so a successful
	# ping from srupf1 to the End.MAP SID demonstrates that the action
	# replaced the destination address with 2001:db8:2::e.
	if ! ip netns exec "$srupf1" ping -6 -c 1 -W "$TIMEOUT" \
			2001:db8:f::1 >/dev/null 2>&1; then
		return 1
	fi
	return 0
}

main()
{
	check_dependencies
	setup

	if run_test; then
		echo "TEST: End.MAP [PASS]"; exit "$ksft_pass"
	else
		echo "TEST: End.MAP [FAIL]"; exit "$ksft_fail"
	fi
}

main "$@"
