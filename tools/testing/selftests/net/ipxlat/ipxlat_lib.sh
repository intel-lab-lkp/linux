#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
#
# Copyright (C) 2026- Mandelbit SRL
# Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
#
#  Author:	Antonio Quartulli <antonio@mandelbit.com>
#		Daniel Gröber <dxld@darkboxed.org>
#		Ralf Lici <ralf@mandelbit.com>

set -o pipefail

IPXLAT_TEST_DIR=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
source "$IPXLAT_TEST_DIR/../lib.sh"

KDIR=${KDIR:-$(readlink -f "$IPXLAT_TEST_DIR/../../../../../")}
YNL_CLI="$KDIR/tools/net/ynl/pyynl/cli.py"
YNL_SPEC="$KDIR/Documentation/netlink/specs/ipxlat.yaml"
IPXLAT_IPERF_TIMEOUT=${IPXLAT_IPERF_TIMEOUT:-10}

IPXLAT_TRANSLATOR_DEV=ipxl0
IPXLAT_VETH4_HOST=veth4r
IPXLAT_VETH4_NS=veth4n
IPXLAT_VETH6_HOST=veth6r
IPXLAT_VETH6_NS=veth6n

IPXLAT_XLAT_PREFIX6=2001:db8:100::
IPXLAT_XLAT_PREFIX6_LEN=40
IPXLAT_XLAT_PREFIX6_HEX=20010db8010000000000000000000000
IPXLAT_LOWEST_IPV6_MTU=1280

IPXLAT_HOST4_ADDR=198.51.100.1
IPXLAT_HOST6_ADDR=2001:db8:1::1

IPXLAT_NS4_ADDR=198.51.100.2
IPXLAT_NS6_ADDR=2001:db8:1::2
export IPXLAT_V4_REMOTE=192.0.2.33

IPXLAT_V6_REMOTE=2001:db8:1c0:2:21::
IPXLAT_V6_NS4=2001:db8:1c6:3364:2::
IPXLAT_V6_NS6_SRC=2001:db8:1c0:2:2::

NS4=""
NS6=""

ipxlat_ynl()
{
	python3 "$YNL_CLI" --spec "$YNL_SPEC" "$@"
}

ipxlat_build_dev_set_json()
{
	local ifindex="$1"

	jq -cn \
		--argjson ifindex "$ifindex" \
		--arg prefix "$IPXLAT_XLAT_PREFIX6_HEX" \
		--argjson prefix_len "$IPXLAT_XLAT_PREFIX6_LEN" \
		--argjson lowest_ipv6_mtu "$IPXLAT_LOWEST_IPV6_MTU" \
			'{
				ifindex: $ifindex,
				config: {
					"xlat-prefix6": {
						prefix: $prefix,
						"prefix-len": $prefix_len
					},
					"lowest-ipv6-mtu": $lowest_ipv6_mtu
				}
			}'
}

ipxlat_require_root()
{
	if [[ $(id -u) -ne 0 ]]; then
		echo "ipxlat selftests need root; skipping"
		exit "$ksft_skip"
	fi
}

ipxlat_require_tools()
{
	if [[ ! -f "$YNL_CLI" || ! -f "$YNL_SPEC" ]]; then
		log_test_skip "ipxlat netlink spec/ynl not found"
		exit "$ksft_skip"
	fi

	for tool in ip python3 ping iperf3 tcpdump timeout jq; do
		require_command "$tool"
	done
}

ipxlat_cleanup()
{
	cleanup_ns "${NS4:-}" "${NS6:-}" || true
	ip link del "$IPXLAT_TRANSLATOR_DEV" 2>/dev/null || true
	ip link del "$IPXLAT_VETH4_HOST" 2>/dev/null || true
	ip link del "$IPXLAT_VETH6_HOST" 2>/dev/null || true
}

# Test topology:
#
# host namespace:
#   - owns ipxlat dev `ipxl0`
#   - has veth peers `veth4r` and `veth6r`
#   - routes IPv4 test prefix (192.0.2.0/24) to ipxl0 (v4 network steering rule)
#   - routes xlat-prefix6 prefix (2001:db8:100::/40) out to NS6 side
#   - routes mapped NS4 IPv6 identity (2001:db8:1c6:3364:2::/128) to ipxl0
#     so NS6->NS4 traffic enters 6->4 translation
#
# NS4:
#   - IPv4-only endpoint: 198.51.100.2/24 on veth4n
#   - default route via host 198.51.100.1 (veth4r)
#   - sends traffic to 192.0.2.33 (translated by ipxl0 to IPv6)
#
# NS6:
#   - IPv6 endpoint: 2001:db8:1::2/64 on veth6n
#   - also owns mapped addresses used by tests:
#       2001:db8:1c0:2:21::  (maps to 192.0.2.33)
#       2001:db8:1c0:2:2::   (maps to 192.0.2.2, used as explicit src
#                             since we have multiple v6 addresses)
#   - route to mapped NS4 IPv6 address is pinned via host:
#       2001:db8:1c6:3364:2::/128
#     This keeps the 6->4 test path deterministic.
#
# ipxlat config under test:
#   - xlat-prefix6 = 2001:db8:100::/40
#   - lowest-ipv6-mtu = 1280
ipxlat_configure_topology()
{
	local ifindex
	local dev_set_json

	if ! ip link add "$IPXLAT_TRANSLATOR_DEV" type ipxlat; then
		echo "ipxlat link kind unavailable; skipping"
		exit "$ksft_skip"
	fi
	ip link set "$IPXLAT_TRANSLATOR_DEV" up
	ifindex=$(cat /sys/class/net/"$IPXLAT_TRANSLATOR_DEV"/ifindex)
	dev_set_json=$(ipxlat_build_dev_set_json "$ifindex")

	if ! ipxlat_ynl --do dev-set --json "$dev_set_json" >/dev/null; then
		echo "ipxlat dev-set failed"
		exit "$ksft_fail"
	fi

	setup_ns NS4 NS6 || exit "$ksft_skip"

	ip link add "$IPXLAT_VETH4_HOST" type veth peer name "$IPXLAT_VETH4_NS"
	ip link add "$IPXLAT_VETH6_HOST" type veth peer name "$IPXLAT_VETH6_NS"
	ip link set "$IPXLAT_VETH4_NS" netns "$NS4"
	ip link set "$IPXLAT_VETH6_NS" netns "$NS6"

	ip addr add "$IPXLAT_HOST4_ADDR/24" dev "$IPXLAT_VETH4_HOST"
	ip -6 addr add "$IPXLAT_HOST6_ADDR/64" dev "$IPXLAT_VETH6_HOST"
	ip link set "$IPXLAT_VETH4_HOST" up
	ip link set "$IPXLAT_VETH6_HOST" up

	ip netns exec "$NS4" ip addr add "$IPXLAT_NS4_ADDR/24" \
		dev "$IPXLAT_VETH4_NS"
	ip netns exec "$NS4" ip link set "$IPXLAT_VETH4_NS" up
	ip netns exec "$NS4" ip route add default via "$IPXLAT_HOST4_ADDR"

	ip netns exec "$NS6" ip -6 addr add "$IPXLAT_NS6_ADDR/64" \
		dev "$IPXLAT_VETH6_NS"
	ip netns exec "$NS6" ip -6 addr add "$IPXLAT_V6_REMOTE/128" \
		dev "$IPXLAT_VETH6_NS"
	ip netns exec "$NS6" ip -6 addr add "$IPXLAT_V6_NS6_SRC/128" \
		dev "$IPXLAT_VETH6_NS"
	ip netns exec "$NS6" ip link set "$IPXLAT_VETH6_NS" up
	ip netns exec "$NS6" ip -6 route add default via "$IPXLAT_HOST6_ADDR"
	ip netns exec "$NS6" ip -6 route replace "$IPXLAT_V6_NS4/128" \
		via "$IPXLAT_HOST6_ADDR"
	sleep 2

	sysctl -qw net.ipv4.ip_forward=1
	sysctl -qw net.ipv6.conf.all.forwarding=1

	# 4->6 steering rule
	ip route replace 192.0.2.0/24 dev "$IPXLAT_TRANSLATOR_DEV"
	# Post-translation egress:
	# IPv6 destinations in xlat-prefix6 leave toward NS6.
	ip -6 route replace "$IPXLAT_XLAT_PREFIX6/$IPXLAT_XLAT_PREFIX6_LEN" \
		dev "$IPXLAT_VETH6_HOST"
	# 6->4 steering rule
	ip -6 route replace "$IPXLAT_V6_NS4/128" dev "$IPXLAT_TRANSLATOR_DEV"

	ip link set "$IPXLAT_VETH6_HOST" mtu 1280
	ip netns exec "$NS6" ip link set "$IPXLAT_VETH6_NS" mtu 1280
}

ipxlat_setup_env()
{
	ipxlat_require_root
	ipxlat_require_tools
	ipxlat_cleanup

	ipxlat_configure_topology
}

ipxlat_run_iperf()
{
	local srv_ns="$1"
	local cli_ns="$2"
	local dst="$3"
	local port="$4"
	local -a args=()
	local client_rc
	local server_rc
	local spid
	local idx

	for ((idx = 5; idx <= $#; idx++)); do
		args+=("${!idx}")
	done

	ip netns exec "$srv_ns" timeout "$IPXLAT_IPERF_TIMEOUT" \
		iperf3 -s -1 -p "$port" >/dev/null 2>&1 &
	spid=$!
	sleep 0.2

	ip netns exec "$cli_ns" timeout "$IPXLAT_IPERF_TIMEOUT" \
		iperf3 -c "$dst" -p "$port" "${args[@]}" >/dev/null 2>&1

	client_rc=$?
	if [[ $client_rc -ne 0 ]]; then
		kill "$spid" >/dev/null 2>&1 || true
	fi

	wait "$spid" >/dev/null 2>&1
	server_rc=$?

	((client_rc != 0)) && return "$client_rc"
	return "$server_rc"
}

ipxlat_capture_pkts()
{
	local ns="$1"
	local filter="$2"
	local expect_pkts="$3"
	local timeout_s="$4"
	local cap_goal
	local cap_pid
	local rc
	local trigger_rc

	shift 4

	cap_goal=1
	[[ $expect_pkts -gt 0 ]] && cap_goal=$expect_pkts

	ip netns exec "$ns" timeout "$timeout_s" \
		tcpdump -nni any -c "$cap_goal" \
		"$filter" >/dev/null 2>&1 &
	cap_pid=$!
	sleep 0.2

	"$@"
	trigger_rc=$?
	wait "$cap_pid" >/dev/null 2>&1
	rc=$?

	if [[ $trigger_rc -ne 0 ]]; then
		return "$trigger_rc"
	fi

	if [[ $expect_pkts -eq 0 ]]; then
		[[ $rc -eq 124 ]]
	else
		[[ $rc -eq 0 ]]
	fi
}
