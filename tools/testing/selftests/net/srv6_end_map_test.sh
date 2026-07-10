#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Selftest for the SRv6 End.MAP behavior (RFC 9433).
#
#   +------+    2001:db8:1::/64    +------+    2001:db8:2::/64    +------+
#   | rt-1 | --------------------- | rt-2 | --------------------- | rt-3 |
#   +------+         veth1         +------+         veth2         +------+
#                                 (End.MAP)
#
# rt-2 holds the End.MAP route for 2001:db8:f::/64 that replaces the
# IPv6 destination with 2001:db8:3::3 (an address on rt-3's loopback,
# also used as the final SRv6 segment in the SRH-present scenarios).
#
# The original destination 2001:db8:f::1 and the replacement
# 2001:db8:3::3 have different 16-bit word sums, so any regression in
# the transport-checksum diff update would corrupt the ICMPv6
# checksum and bump Icmp6InCsumErrors -- this test asserts that the
# counter stays at zero across the run.
#
# Five cases are exercised:
#
#   1. SRH absent     -- plain ICMPv6 echo to the End.MAP SID.
#   2. SRH present    -- the destination is reached through an
#                        H.Encaps wrapper that carries an SRH with
#                        two segments; End.MAP must leave the SRH
#                        structurally intact.
#   3. SRH inline     -- the destination is reached through an
#                        H.Insert wrapper that inserts an SRH whose
#                        first hop is the End.MAP SID; End.MAP must
#                        NOT patch the L4 checksum, because the
#                        receiver's SRv6 processing restores the
#                        destination from segments[0] before the
#                        ICMPv6 handler verifies it.
#   4. SRH malformed  -- a C helper sends a packet whose Routing
#                        Header type is not 4; End.MAP must drop it.
#                        The behavior's own errors counter binds the
#                        assertion to the drop.
#   5. Hop Limit      -- an echo whose Hop Limit is 1 on arrival at
#                        the End.MAP node must yield an ICMPv6 Time
#                        Exceeded from that node, confirming Hop Limit
#                        handling is delegated to the ip6_forward path.

source lib.sh

readonly PING_TIMEOUT_SEC=4
readonly END_MAP_PREFIX="2001:db8:f::/64"
readonly END_MAP_SID="2001:db8:f::1"
readonly RT3_SID="2001:db8:3::3"
HELPER_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly HELPER_DIR
readonly HELPER="${HELPER_DIR}/srv6_mobile_send"

ret=0
nsuccess=0
nfail=0

PAUSE_ON_FAIL=${PAUSE_ON_FAIL:=no}

log_test()
{
	local rc=$1
	local expected=$2
	local msg="$3"

	if [ "${rc}" -eq "${expected}" ]; then
		nsuccess=$((nsuccess + 1))
		printf "\n    TEST: %-60s  [ OK ]\n" "${msg}"
	else
		ret=1
		nfail=$((nfail + 1))
		printf "\n    TEST: %-60s  [FAIL]\n" "${msg}"
		if [ "${PAUSE_ON_FAIL}" = "yes" ]; then
			echo
			echo "hit enter to continue, 'q' to quit"
			read -r a
			[ "$a" = "q" ] && exit 1
		fi
	fi
}

print_log_test_results()
{
	printf "\nTests passed: %3d\n" "${nsuccess}"
	printf "Tests failed: %3d\n"   "${nfail}"
}

cleanup()
{
	cleanup_all_ns
}

trap cleanup EXIT

check_dependencies()
{
	if [ "$(id -u)" -ne 0 ]; then
		echo "SKIP: need root privileges"
		exit "${ksft_skip}"
	fi

	for cmd in ip ping; do
		if ! command -v "$cmd" >/dev/null; then
			echo "SKIP: ${cmd} is required"
			exit "${ksft_skip}"
		fi
	done

	if [ ! -x "${HELPER}" ]; then
		echo "SKIP: ${HELPER} not built"
		exit "${ksft_skip}"
	fi

	if ! ip route help 2>&1 | grep -qF "seg6mobile"; then
		echo "SKIP: iproute2 lacks seg6mobile support"
		exit "${ksft_skip}"
	fi

	if ! ip route help 2>&1 | grep -qF "End.MAP"; then
		echo "SKIP: iproute2 lacks End.MAP action"
		exit "${ksft_skip}"
	fi
}

setup()
{
	setup_ns rt1 rt2 rt3

	ip -n "$rt1" link set lo up
	ip -n "$rt2" link set lo up
	ip -n "$rt3" link set lo up

	ip link add veth1 netns "$rt1" \
		type veth peer name veth1-rt2 netns "$rt2"
	ip link add veth2 netns "$rt2" \
		type veth peer name veth2-rt3 netns "$rt3"

	ip -n "$rt1" addr add 2001:db8:1::1/64 dev veth1 nodad
	ip -n "$rt2" addr add 2001:db8:1::2/64 dev veth1-rt2 nodad
	ip -n "$rt2" addr add 2001:db8:2::1/64 dev veth2 nodad
	ip -n "$rt3" addr add 2001:db8:2::2/64 dev veth2-rt3 nodad
	# rt-3 also owns the End.MAP replacement SID / SRH endpoint.
	ip -n "$rt3" addr add "$RT3_SID/128" dev lo nodad

	ip -n "$rt1" link set veth1 up
	ip -n "$rt2" link set veth1-rt2 up
	ip -n "$rt2" link set veth2 up
	ip -n "$rt3" link set veth2-rt3 up

	ip netns exec "$rt2" sysctl -wq net.ipv6.conf.all.forwarding=1

	# rt-3 must accept SRv6 packets so ipv6_srh_rcv lets the
	# extension header chain through to local delivery.
	ip netns exec "$rt3" sysctl -wq net.ipv6.conf.all.seg6_enabled=1
	ip netns exec "$rt3" \
		sysctl -wq net.ipv6.conf.veth2-rt3.seg6_enabled=1
	ip netns exec "$rt3" sysctl -wq net.ipv6.conf.lo.seg6_enabled=1

	# Disable HW checksum offload so the kernel software checksum
	# path runs unconditionally and any csum bug surfaces.
	ip netns exec "$rt1" ethtool -K veth1 tx off rx off 2>/dev/null
	ip netns exec "$rt2" ethtool -K veth1-rt2 tx off rx off \
		2>/dev/null
	ip netns exec "$rt2" ethtool -K veth2 tx off rx off 2>/dev/null
	ip netns exec "$rt3" ethtool -K veth2-rt3 tx off rx off \
		2>/dev/null

	# rt-1: route the End.MAP locator into rt-2.
	ip -n "$rt1" -6 route add "$END_MAP_PREFIX" via 2001:db8:1::2

	# rt-1: a separate H.Encaps route for the SRH-present scenario,
	# wrapping the inner ICMPv6 echo in an outer IPv6+SRH carrying
	# [End.MAP_SID, RT3_SID].
	ip -n "$rt1" -6 route add "$RT3_SID/128" via 2001:db8:1::2 \
		encap seg6 mode encap \
		segs "$END_MAP_SID","$RT3_SID" \
		dev veth1

	# rt-2: End.MAP -- swap DA from the End.MAP SID to RT3_SID
	# (an address on rt-3) and forward via the IPv6 FIB.  "count"
	# enables the per-behavior counters the malformed-SRH test reads.
	ip -n "$rt2" -6 route add "$END_MAP_PREFIX" \
		encap seg6mobile action End.MAP nh6 "$RT3_SID" count \
		dev veth2

	# rt-2: reach RT3_SID (on rt-3's loopback) through the
	# directly connected neighbour 2001:db8:2::2.
	ip -n "$rt2" -6 route add "$RT3_SID/128" via 2001:db8:2::2

	# rt-3: return route for the ICMPv6 echo reply.
	ip -n "$rt3" -6 route add 2001:db8:1::/64 via 2001:db8:2::1
}

read_nstat_counter()
{
	local ns=$1
	local name=$2

	# nstat -az reports a counter that has never incremented as 0,
	# which is what we rely on for a clean before/after delta.
	ip netns exec "$ns" nstat -az "$name" \
		| awk -v n="$name" '$1 == n {print $2}'
}

read_route_errors()
{
	# The End.MAP route carries "count", so its errors counter
	# increments once for every packet the behavior drops.  Reading it
	# binds the negative test to the drop itself rather than to any
	# unrelated loss on the path to rt-3.
	ip -n "$rt2" -j -s -6 route show "$END_MAP_PREFIX" \
		| grep -oE '"errors":[0-9]+' | grep -oE '[0-9]+'
}

# Test 1: SRH absent.
test_srh_absent()
{
	local before after rc=0

	before=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)

	if ! ip netns exec "$rt1" \
			ping -6 -c 1 -W "$PING_TIMEOUT_SEC" "$END_MAP_SID" \
			>/dev/null 2>&1; then
		rc=1
	fi

	if [ "$rc" -eq 0 ]; then
		after=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)
		[ "$before" != "$after" ] && rc=1
	fi

	log_test "$rc" 0 "End.MAP forwards an ICMPv6 echo without an SRH"
}

# Test 2: SRH present (H.Encaps from rt-1).
#
# The packet rt-1 emits carries an SRH whose only intermediate
# segment is the End.MAP SID; the final segment is RT3_SID.  End.MAP
# rewrites the outer destination from END_MAP_SID to RT3_SID
# without touching the SRH, so on rt-3 the SRH processing advances
# to segments[0] = RT3_SID (local) and the encapsulated inner
# packet is decapsulated and replied to.  A regression that mangles
# the SRH (e.g. flipping bits in segments_left) would break the chain
# and the ping would not return.
test_srh_present()
{
	local before after rc=0

	before=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)

	if ! ip netns exec "$rt1" \
			ping -6 -c 1 -W "$PING_TIMEOUT_SEC" "$RT3_SID" \
			>/dev/null 2>&1; then
		rc=1
	fi

	if [ "$rc" -eq 0 ]; then
		after=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)
		[ "$before" != "$after" ] && rc=1
	fi

	log_test "$rc" 0 "End.MAP preserves an SRH carried by H.Encaps"
}

# Test 3: SRH inserted in-place by H.Insert (mode inline).
#
# rt-1 inserts an SRH between the IPv6 header and the L4 payload;
# segments[0] is the original DA (2001:db8:2::2), segments[1] is the
# End.MAP SID.  End.MAP rewrites the outer DA to RT3_SID but must
# NOT patch the L4 checksum, because the kernel's standard SRv6
# processing on rt-3 will decrement Segments Left to 0 and restore
# the destination to segments[0] before the ICMPv6 handler verifies
# the checksum against the original pseudo-header.  Over-patching the
# checksum would corrupt it and bump Icmp6InCsumErrors.
test_srh_inline()
{
	local before after rc=0

	ip -n "$rt1" -6 route add 2001:db8:2::2/128 via 2001:db8:1::2 \
		encap seg6 mode inline segs "$END_MAP_SID" \
		dev veth1

	before=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)

	if ! ip netns exec "$rt1" \
			ping -6 -c 1 -W "$PING_TIMEOUT_SEC" 2001:db8:2::2 \
			>/dev/null 2>&1; then
		rc=1
	fi

	if [ "$rc" -eq 0 ]; then
		after=$(read_nstat_counter "$rt3" Icmp6InCsumErrors)
		[ "$before" != "$after" ] && rc=1
	fi

	log_test "$rc" 0 "End.MAP preserves L4 csum across mode inline SRH"
}

# Test 4: SRH malformed (negative test).
#
# A C helper crafts an IPv6 packet whose Routing Header type is 0
# rather than 4 (SRH), then sends it from rt-1 toward the End.MAP
# SID.  End.MAP's seg6_mobile_get_and_validate_srh() must return
# MALFORMED and the handler must drop the packet.  The assertion reads
# the behavior's own errors counter, so it is satisfied only by the
# End.MAP drop and not by any unrelated loss.
test_srh_malformed()
{
	local before after rc=0

	before=$(read_route_errors)

	ip netns exec "$rt1" "$HELPER" 2001:db8:1::1 "$END_MAP_SID" \
		>/dev/null 2>&1

	after=$(read_route_errors)
	[ "$((after - before))" -eq 1 ] || rc=1

	log_test "$rc" 0 "End.MAP drops a packet carrying a malformed SRH"
}

# Test 5: Hop Limit expiry.
#
# End.MAP delegates the Hop Limit check and decrement to the ip6_forward
# path.  An echo whose Hop Limit is 1 when it reaches rt-2 must elicit
# an ICMPv6 Time Exceeded from rt-2 instead of being forwarded, so
# rt-2's Icmp6OutTimeExcds increments by exactly one.
test_hoplimit_expiry()
{
	local before after rc=0

	before=$(read_nstat_counter "$rt2" Icmp6OutTimeExcds)

	ip netns exec "$rt1" \
		ping -6 -c 1 -t 1 -W "$PING_TIMEOUT_SEC" "$END_MAP_SID" \
		>/dev/null 2>&1

	after=$(read_nstat_counter "$rt2" Icmp6OutTimeExcds)
	[ "$((after - before))" -eq 1 ] || rc=1

	log_test "$rc" 0 "End.MAP delegates Hop Limit expiry to ip6_forward"
}

main()
{
	check_dependencies
	setup

	test_srh_absent
	test_srh_present
	test_srh_inline
	test_srh_malformed
	test_hoplimit_expiry

	print_log_test_results
	exit "${ret}"
}

main "$@"
