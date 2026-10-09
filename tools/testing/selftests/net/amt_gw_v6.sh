#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Netlink coverage for the AMT IPv6 attributes, IFLA_AMT_LOCAL_IP6 and
# IFLA_AMT_DISCOVERY_IP6, and the rules that validate them. amt_v6.sh covers
# the data plane; this needs only one netns and one dummy device.
#
# Not covered: `discovery <v4>` together with `discovery <v6>`, or `local <v4>`
# together with `local <v6>`, in one command. iproute2 rejects the duplicate
# argument itself, so those checks are reachable only from a raw netlink
# client.

source lib.sh

readonly V6_LOCAL="2001:db8:a::1"
readonly V6_DISC="2001:db8:a::2"
readonly V4_LOCAL="192.168.0.1"
readonly V4_DISC="192.168.0.2"

# add_amt <name> <args...>: create an amt link on gw_dev, stderr in $ADD_ERR.
add_amt()
{
	local name=$1; shift

	ADD_ERR=$(ip -n "$GW" link add "$name" type amt dev gw_dev "$@" 2>&1)
}

# amt_field <name> <key>: one attribute as amt_fill_info reports it.
amt_field()
{
	ip -n "$GW" -d -j link show "$1" 2>/dev/null |
		jq -r ".[0].linkinfo.info_data.$2 // empty"
}

# An iproute2 without IFLA_AMT_DISCOVERY_IP6 may not reject `discovery
# <v6>`: it can pack the literal into IFLA_AMT_DISCOVERY_IP, which this
# kernel refuses and an older one turns into a v4 gateway from the first
# four bytes. So probe on the readback, not only on the exit code. A
# kernel without the IPv6 attributes ignores them and asks for a local
# address. Skip in those cases only; any other failure is a failure of the
# code under test.
probe_v6_gateway()
{
	local got

	if ! add_amt amtprobe mode gateway local "$V6_LOCAL" \
	     discovery "$V6_DISC"; then
		case "$ADD_ERR" in
		*"IPv6 address in an IPv4 attribute"*)
			# An older iproute2 on this kernel: the rule that
			# refuses the 16-byte IPv4 attribute is what ran.
			RET=0
			log_test "16-byte IPv4 address attribute is refused"
			echo "SKIP: iproute2 lacks IPv6 AMT support"
			exit "$ksft_skip"
			;;
		*"Local attribute is required"*|*"expected rather than"*|\
		*"IPv6 support is disabled"*)
			echo "SKIP: no IPv6 AMT support: $ADD_ERR"
			exit "$ksft_skip"
			;;
		esac
		echo "FAIL: cannot create an IPv6 gateway: $ADD_ERR"
		exit "$ksft_fail"
	fi
	got=$(amt_field amtprobe discovery)
	ip -n "$GW" link del amtprobe
	if [[ "$got" != *:* ]]; then
		# A kernel with the IPv6 attributes names the missing
		# discovery address this way, and must have refused the
		# 16-byte IPv4 attribute that an older iproute2 sent.
		add_amt amtprobe mode gateway local "$V4_LOCAL"
		ip -n "$GW" link del amtprobe 2>/dev/null
		if [[ "$ADD_ERR" == *"of the local family is required"* ]]; then
			echo "FAIL: 16-byte IPv4 attribute read back as '$got'"
			exit "$ksft_fail"
		fi
		echo "SKIP: iproute2 lacks IPv6 AMT support (read back '$got')"
		exit "$ksft_skip"
	fi
}

test_v6_gateway_roundtrip()
{
	RET=0
	add_amt amtg6 mode gateway local "$V6_LOCAL" discovery "$V6_DISC"
	check_err $? "create failed: $ADD_ERR"
	[ "$(amt_field amtg6 discovery)" = "$V6_DISC" ] ||
		check_err 1 "discovery did not read back as $V6_DISC"
	[ "$(amt_field amtg6 local)" = "$V6_LOCAL" ] ||
		check_err 1 "local did not read back as $V6_LOCAL"
	ip -n "$GW" link del amtg6 2>/dev/null
	log_test "v6 gateway: discovery round-trips through fill_info"
}

test_v6_relay_roundtrip()
{
	RET=0
	add_amt amtr6 mode relay local "$V6_LOCAL"
	check_err $? "create failed: $ADD_ERR"
	[ "$(amt_field amtr6 local)" = "$V6_LOCAL" ] ||
		check_err 1 "local did not read back as $V6_LOCAL"
	[ -z "$(amt_field amtr6 discovery)" ] ||
		check_err 1 "a relay reported a discovery address"
	ip -n "$GW" link del amtr6 2>/dev/null
	log_test "v6 relay: local round-trips, no discovery reported"
}

test_v4_gateway_unchanged()
{
	RET=0
	add_amt amtg4 mode gateway local "$V4_LOCAL" discovery "$V4_DISC"
	check_err $? "create failed: $ADD_ERR"
	[ "$(amt_field amtg4 discovery)" = "$V4_DISC" ] ||
		check_err 1 "discovery did not read back as $V4_DISC"
	ip -n "$GW" link del amtg4 2>/dev/null
	log_test "v4 gateway still creates (no ABI change)"
}

# An IPv4 relay has always ignored IFLA_AMT_DISCOVERY_IP, and still must.
test_v4_relay_unchanged()
{
	RET=0
	add_amt amtr4 mode relay local "$V4_LOCAL" discovery "$V4_DISC"
	check_err $? "create failed: $ADD_ERR"
	ip -n "$GW" link del amtr4 2>/dev/null
	log_test "v4 relay still accepts a v4 discovery (no ABI change)"
}

# expect_reject <description> <extack substring> <args...>: the create must
# fail, and for the stated reason.
expect_reject()
{
	local desc=$1 want=$2; shift 2

	RET=0
	add_amt amtbad "$@"
	check_fail $? "link was created but should have been rejected"
	ip -n "$GW" link del amtbad 2>/dev/null
	grep -qi -- "$want" <<< "$ADD_ERR"
	check_err $? "rejected for the wrong reason: $ADD_ERR"
	log_test "$desc"
}

require_command jq
if [ ! -e /proc/net/if_inet6 ]; then
	echo "SKIP: the kernel has no IPv6"
	exit "$ksft_skip"
fi
trap cleanup_all_ns EXIT
setup_ns GW || exit "$ksft_skip"
ip -n "$GW" link add gw_dev type dummy
ip -n "$GW" link set gw_dev up

probe_v6_gateway

test_v6_gateway_roundtrip
test_v6_relay_roundtrip
test_v4_gateway_unchanged
test_v4_relay_unchanged
expect_reject "gateway without discovery is rejected" \
	"Discovery attribute" \
	mode gateway local "$V6_LOCAL"
expect_reject "gateway v6 local + v4 discovery is rejected" \
	"of the local family is required" \
	mode gateway local "$V6_LOCAL" discovery "$V4_DISC"
expect_reject "gateway v4 local + v6 discovery is rejected" \
	"of the local family is required" \
	mode gateway local "$V4_LOCAL" discovery "$V6_DISC"
expect_reject "v6 discovery in relay mode is rejected" \
	"only valid in gateway mode" \
	mode relay local "$V6_LOCAL" discovery "$V6_DISC"
expect_reject "v4 discovery on a v6 relay is rejected" \
	"only valid in gateway mode" \
	mode relay local "$V6_LOCAL" discovery "$V4_DISC"
expect_reject "v6 discovery on a v4 relay is rejected" \
	"only valid in gateway mode" \
	mode relay local "$V4_LOCAL" discovery "$V6_DISC"
expect_reject "unspecified v6 local is rejected" \
	"Invalid Local IPv6" \
	mode relay local ::
expect_reject "v4-mapped v6 local is rejected" \
	"Invalid Local IPv6" \
	mode relay local ::ffff:192.168.0.1
expect_reject "loopback v6 local is rejected" \
	"Invalid Local IPv6" \
	mode relay local ::1
expect_reject "multicast v6 local is rejected" \
	"Invalid Local IPv6" \
	mode relay local ff02::1
expect_reject "multicast v6 discovery is rejected" \
	"must be unicast" \
	mode gateway local "$V6_LOCAL" discovery ff02::1
expect_reject "loopback v6 discovery is rejected" \
	"must be unicast" \
	mode gateway local "$V6_LOCAL" discovery ::1
expect_reject "unspecified v6 discovery is rejected" \
	"must be unicast" \
	mode gateway local "$V6_LOCAL" discovery ::
expect_reject "v4-mapped v6 discovery is rejected" \
	"must be unicast" \
	mode gateway local "$V6_LOCAL" discovery ::ffff:192.168.0.2

exit "$EXIT_STATUS"
