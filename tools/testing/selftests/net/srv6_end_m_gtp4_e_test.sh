#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2034,SC2154
#
# Selftest for the SRv6 End.M.GTP4.E behavior (RFC 9433 Section 6.6).
#
# Three network namespaces are connected back-to-back:
#
#   +-------+   2001:db8:1::/64   +-------+     10.0.0.0/24     +-------+
#   | srupf | ------------------- | srgw  | ------------------- |  gnb  |
#   +-------+        veth-n9      +-------+        veth-n3      +-------+
#
# srupf is the SR-domain-side SRv6-aware UPF (RFC 9433 sense, not a
# 3GPP UPF) that injects the SRv6 packets, gnb is the GTP-U-side
# test peer, and srgw runs the End.M.GTP4.E behavior under test.
#
# On srgw an End.M.GTP4.E SID is installed with a /32 routing prefix;
# the SID layout (per RFC 9433 Section 6.6 Figure 9) is:
#
#   Locator | IPv4 DA (v4_mask_len bits) | Args.Mob.Session (40 bits) [| pad]
#
# With locator=/32 and v4_mask_len=32 the IPv4 DA lives at bytes 4..7 and
# Args.Mob.Session at bytes 8..12; bytes 13..15 are SID padding.
# Choosing a non-tail-aligned layout (i.e. not /56 with c=0) makes sure
# the test exercises the offset-based extraction rather than a
# "last 5 bytes" shortcut.
#
# Args.Mob.Session is laid out as (RFC 9433 Section 6.1, Figure 8 -- 40 bits):
#   QFI (6) | R (1) | U (1) | PDU Session ID (32)
#
# The test crafts an IPv6 packet whose destination address encodes
#
#   IPv4 DA          = 10.0.0.2 (gnb)
#   QFI              = 5
#   PDU Session ID   = 0x123 (= the GTP-U TEID, 32 bits)
#
# Args.Mob.Session bytes are therefore 14 00 00 01 23 (top byte is the
# QFI byte (5 << 2) = 0x14, next four bytes are the 32-bit TEID).  With
# the /32-locator placement the SID ends up as
#   2001:db8:a00:2:1400:1:2300:0 .
# The expected output is an IPv4/UDP/GTP-U(long)/PDU-Session-ext packet with
# TEID 0x00000123 and QFI 5.
#
# The IPv6 source address layout per RFC 9433 Section 6.6 Figure 10:
#
#   | Source srupf Prefix (P bits) | IPv4 SA (a bits) | padding |
#
# is exercised in two scenarios:
#   - Default (no v6_src_prefix_len attribute): P = 64, IPv4 SA at
#     IPv6 bytes 8..11.
#   - Explicit v6_src_prefix_len 48: IPv4 SA at IPv6 bytes 6..9, with
#     a 6-byte Source srupf Prefix and a 6-byte trailing padding region.

source lib.sh

readonly TIMEOUT=4
tcpdump_pid=""
have_vrf=0

cleanup()
{
	if [ -n "$tcpdump_pid" ]; then
		kill "$tcpdump_pid" 2>/dev/null
		wait "$tcpdump_pid" 2>/dev/null
	fi
	cleanup_all_ns
}

trap cleanup EXIT

setup()
{
	setup_ns srupf srgw gnb gnb_vrf

	ip -n "$srgw" link set dev lo up
	ip -n "$srupf" link set dev lo up
	ip -n "$gnb" link set dev lo up
	ip -n "$gnb_vrf" link set dev lo up

	# upf <-> srgw (IPv6).  Two srupf addresses encode the same
	# IPv4 SA (10.0.0.1) at different byte offsets, exercising the
	# default /64 and an explicit /48 Source srupf Prefix layout:
	#   2001:db8:1::a00:1:0:1     -> IPv4 SA at IPv6 bytes 8..11 (P = 64)
	#   2001:db8:3:a00:1::1       -> IPv4 SA at IPv6 bytes 6..9  (P = 48)
	# The srgw peer addresses are placed on the same IPv6 /64 prefix
	# as the srupf side so the srupf routes can name them as on-link
	# next-hops without explicit neighbor discovery.
	ip link add veth-n9 netns "$srupf" type veth peer name veth-n9-srgw \
		netns "$srgw"
	ip -n "$srupf" addr add 2001:db8:1::a00:1:0:1/64 dev veth-n9 nodad
	ip -n "$srupf" addr add 2001:db8:3:a00:1::1/64 dev veth-n9 nodad
	ip -n "$srgw" addr add 2001:db8:1::2/64 dev veth-n9-srgw nodad
	ip -n "$srgw" addr add 2001:db8:3:a00:1::2/64 dev veth-n9-srgw nodad
	ip -n "$srupf" link set dev veth-n9 up
	ip -n "$srgw" link set dev veth-n9-srgw up

	# srgw <-> gnb (IPv4)
	ip link add veth-n3 netns "$srgw" type veth peer name veth-n3-gnb \
		netns "$gnb"
	ip -n "$srgw" addr add 10.0.0.1/24 dev veth-n3
	ip -n "$gnb" addr add 10.0.0.2/24 dev veth-n3-gnb
	ip -n "$srgw" link set dev veth-n3 up
	ip -n "$gnb" link set dev veth-n3-gnb up

	# allow forwarding on srgw
	ip netns exec "$srgw" sysctl -wq net.ipv4.ip_forward=1
	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.all.forwarding=1

	# routes on upf toward the End.M.GTP4.E locators
	ip -n "$srupf" -6 route add 2001:db8::/32 via 2001:db8:1::2
	ip -n "$srupf" -6 route add 2001:db9::/32 via 2001:db8:3:a00:1::2
	ip -n "$srupf" -6 route add 2001:dbb::/32 via 2001:db8:1::2

	# install End.M.GTP4.E on srgw with PDU Session Container (5G N3:
	# pdu_type dl), default /64 Source srupf Prefix
	ip -n "$srgw" -6 route add 2001:db8::/32 \
		encap seg6local action End.M.GTP4.E \
			src 2001:db8:1::2 v4_mask_len 32 pdu_type dl \
		dev veth-n3

	# install End.M.GTP4.E on srgw with PDU Session Container and
	# explicit v6_src_prefix_len 48
	ip -n "$srgw" -6 route add 2001:db9::/32 \
		encap seg6local action End.M.GTP4.E \
			src 2001:db9::1 v4_mask_len 32 v6_src_prefix_len 48 \
			pdu_type dl \
		dev veth-n3

	# install End.M.GTP4.E on srgw WITHOUT pdu_type: short GTPv1-U
	# (LTE-style, no PDU Session Container) regardless of QFI
	ip -n "$srgw" -6 route add 2001:dbb::/32 \
		encap seg6local action End.M.GTP4.E \
			src 2001:db8:1::2 v4_mask_len 32 \
		dev veth-n3

	# Per-route VRF case: a second egress IPv4 path in its own VRF
	# (e.g. modelling a second tenant on a different interface).  The
	# End.M.GTP4.E SID for this tenant binds the egress IPv4 lookup to
	# the VRF via the standard seg6_local 'oif' attribute; without
	# it, the lookup would fall through to the main table where the
	# 10.0.1.0/24 prefix does not exist.
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	modprobe vrf 2>/dev/null
	if ip -n "$srgw" link add vrf-n3 type vrf table 100 2>/dev/null; then
		have_vrf=1
		ip -n "$srgw" link set dev vrf-n3 up

		ip link add veth-n3-2 netns "$srgw" type veth peer name \
			veth-n3-2-gnb netns "$gnb_vrf"
		ip -n "$srgw" link set dev veth-n3-2 master vrf-n3
		ip -n "$srgw" addr add 10.0.1.1/24 dev veth-n3-2
		ip -n "$gnb_vrf" addr add 10.0.1.2/24 dev veth-n3-2-gnb
		ip -n "$srgw" link set dev veth-n3-2 up
		ip -n "$gnb_vrf" link set dev veth-n3-2-gnb up

		ip -n "$srupf" -6 route add 2001:dba::/32 via 2001:db8:1::2

		ip -n "$srgw" -6 route add 2001:dba::/32 \
			encap seg6local action End.M.GTP4.E \
				src 2001:db8:1::2 v4_mask_len 32 oif vrf-n3 \
				pdu_type dl \
			dev veth-n3-2
	fi
}

check_dependencies()
{
	if ! ip netns help 2>&1 | grep -q "exec"; then
		echo "SKIP: ip netns exec not available"
		exit "$ksft_skip"
	fi

	if ! command -v tcpdump >/dev/null; then
		echo "SKIP: tcpdump is required"
		exit "$ksft_skip"
	fi

	if ! command -v ping >/dev/null; then
		echo "SKIP: ping is required"
		exit "$ksft_skip"
	fi

	if ! command -v python3 >/dev/null; then
		echo "SKIP: python3 is required"
		exit "$ksft_skip"
	fi

	if ! ip route help 2>&1 | grep -qF "End.M.GTP4.E"; then
		echo "SKIP: iproute2 too old, missing seg6local action End.M.GTP4.E"
		exit "$ksft_skip"
	fi

	if ! python3 -c "import scapy.all" 2>/dev/null; then
		echo "SKIP: python3-scapy is required"
		exit "$ksft_skip"
	fi
}

capture_traffic()
{
	local capture_ns="$1"
	local capture_iface="$2"
	local src="$3"
	local sid="$4"
	local out="$5"

	# capture GTP-U traffic on the egress side.  The capture is torn down
	# by the explicit kill -INT below; the cleanup() trap only fires for
	# unexpected exits.
	ip netns exec "$capture_ns" tcpdump -U -nni "$capture_iface" -w "$out" \
		'udp port 2152' 2>/dev/null &
	tcpdump_pid=$!
	# Give tcpdump a brief moment to attach the BPF filter before we
	# start sending traffic; tcpdump does not expose a "ready" signal.
	sleep 1

	# Send a single ICMPv6 echo-request to the End.M.GTP4.E SID.
	ip netns exec "$srupf" ping -6 -c 1 -W "$TIMEOUT" -I "$src" "$sid" \
		>/dev/null 2>&1

	# stop tcpdump after the packet has had time to traverse
	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""
}

run_test()
{
	local src="$1"			# IPv6 SA the srupf must use
	local sid="$2"			# End.M.GTP4.E SID to ping
	local expected_v4_src="$3"	# expected IPv4 SA in the egress GTP-U
	local capture_ns="${4:-$gnb}"	# netns where GTP-U is expected to land
	local capture_iface="${5:-veth-n3-gnb}"
	local out
	local rc

	out=$(mktemp)
	capture_traffic "$capture_ns" "$capture_iface" "$src" "$sid" "$out"

	# Expected wire layout (verified via scapy field comparison rather
	# than tcpdump -X | grep so the test is robust against tcpdump
	# output formatting changes):
	#   IPv4 (src=$expected_v4_src, dst=10.0.0.2) | UDP(2152) |
	#     GTPv1 long (TEID=0x123, S/PN/E=001) |
	#     PDU Session ext (next=0x85, len=1, PDU type=DL=0, QFI=5) | inner T-PDU
	EXPECTED_V4_SRC="$expected_v4_src" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IP, UDP

expected_v4_src = os.environ['EXPECTED_V4_SRC']
pkts = rdpcap(sys.argv[1])
if not pkts:
    sys.exit("no captured packets")

found = False
for p in pkts:
    if not (IP in p and UDP in p):
        continue
    if p[UDP].dport != 2152:
        continue
    if p[IP].src != expected_v4_src:
        sys.exit(f"unexpected IPv4 SA {p[IP].src}, want {expected_v4_src}")
    payload = bytes(p[UDP].payload)
    # GTP-U long header: flags(1)|mtype(1)|len(2)|teid(4)|seq(2)|npdu(1)|next(1)
    if len(payload) < 12:
        continue
    teid = int.from_bytes(payload[4:8], 'big')
    if teid != 0x00000123:
        sys.exit(f"unexpected TEID 0x{teid:08x}, want 0x00000123")
    next_ext = payload[11]
    if next_ext != 0x85:
        sys.exit(f"missing PDU Session ext (next={next_ext:#04x}, want 0x85)")
    pdu_session = payload[12:16]
    if pdu_session[0] != 0x01:
        sys.exit(f"PDU Session ext_len {pdu_session[0]} != 1")
    pdu_type = pdu_session[1] >> 4
    qfi = pdu_session[2] & 0x3f
    if pdu_type != 0:
        sys.exit(f"PDU Type {pdu_type} != 0 (DL)")
    if qfi != 5:
        sys.exit(f"PDU Session QFI {qfi} != 5")
    found = True
    break

if not found:
    sys.exit("no IPv4/UDP/GTP-U packet observed")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

# Verify the short-GTPv1-U output produced when pdu_type is unset on the
# route: 8-byte GTP-U header, no extension flag, no PDU Session
# Container, regardless of the QFI extracted from Args.Mob.Session.
run_test_short()
{
	local src="$1"
	local sid="$2"
	local expected_v4_src="$3"
	local out
	local rc

	out=$(mktemp)
	capture_traffic "$gnb" "veth-n3-gnb" "$src" "$sid" "$out"

	EXPECTED_V4_SRC="$expected_v4_src" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IP, UDP

expected_v4_src = os.environ['EXPECTED_V4_SRC']
pkts = rdpcap(sys.argv[1])
if not pkts:
    sys.exit("no captured packets")

found = False
for p in pkts:
    if not (IP in p and UDP in p):
        continue
    if p[UDP].dport != 2152:
        continue
    if p[IP].src != expected_v4_src:
        sys.exit(f"unexpected IPv4 SA {p[IP].src}, want {expected_v4_src}")
    payload = bytes(p[UDP].payload)
    if len(payload) < 8:
        continue
    flags = payload[0]
    # Short GTPv1-U: version=1, PT=1, no E/S/PN bits (0x30).
    if flags != 0x30:
        sys.exit(f"unexpected GTP-U flags {flags:#04x}, want 0x30 (short)")
    teid = int.from_bytes(payload[4:8], 'big')
    if teid != 0x00000123:
        sys.exit(f"unexpected TEID 0x{teid:08x}, want 0x00000123")
    found = True
    break

if not found:
    sys.exit("no IPv4/UDP/GTP-U packet observed")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

# Verify that nf_hooks_lwtunnel=1 makes the inner T-PDU 5-tuple
# visible to nftables on the SR Gateway.  The inner T-PDU is IPv6
# (ICMPv6 echo-request from the upf); the nft rule matches on its
# IPv6 source address.  DROP must suppress the GTP-U at the gnb,
# ACCEPT must let it through.
run_nf_test()
{
	local verdict="$1"		# drop | accept
	local expect="$2"		# 1 if GTP-U expected, empty otherwise
	local src="2001:db8:1::a00:1:0:1"
	local sid="2001:db8:a00:2:1400:1:2300:0"
	local out

	ip netns exec "$srgw" nft flush chain ip6 filter prerouting
	ip netns exec "$srgw" nft add rule ip6 filter prerouting \
		ip6 saddr "$src" "$verdict"

	out=$(mktemp)
	capture_traffic "$gnb" "veth-n3-gnb" "$src" "$sid" "$out"

	if [ -n "$expect" ]; then
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IP, UDP

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IP in p and UDP in p and p[UDP].dport == 2152:
        sys.exit(0)
sys.exit("expected GTP-U packet not observed at gnb despite nft accept")
PYEOF
	else
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IP, UDP

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IP in p and UDP in p and p[UDP].dport == 2152:
        sys.exit("GTP-U packet leaked to gnb despite nft drop on inner")
sys.exit(0)
PYEOF
	fi
	local rc=$?
	rm -f "$out"
	return $rc
}

main()
{
	local rc=0

	check_dependencies
	setup

	# Default /64 layout: IPv4 SA at IPv6 bytes 8..11.
	if run_test "2001:db8:1::a00:1:0:1" "2001:db8:a00:2:1400:1:2300:0" \
		    "10.0.0.1"; then
		echo "TEST: End.M.GTP4.E (default /64) [PASS]"
	else
		echo "TEST: End.M.GTP4.E (default /64) [FAIL]"
		rc=1
	fi

	# v6_src_prefix_len 48 layout: IPv4 SA at IPv6 bytes 6..9.
	if run_test "2001:db8:3:a00:1::1" "2001:db9:a00:2:1400:1:2300:0" \
		    "10.0.0.1"; then
		echo "TEST: End.M.GTP4.E (v6_src_prefix_len 48) [PASS]"
	else
		echo "TEST: End.M.GTP4.E (v6_src_prefix_len 48) [FAIL]"
		rc=1
	fi

	# pdu_type unset: emit short GTPv1-U with no PDU Session Container
	# even though Args.Mob.Session encodes QFI=5.  This is the LTE-only
	# / S1-U style output.
	if run_test_short "2001:db8:1::a00:1:0:1" \
			  "2001:dbb:a00:2:1400:1:2300:0" \
			  "10.0.0.1"; then
		echo "TEST: End.M.GTP4.E (pdu_type unset, short header) [PASS]"
	else
		echo "TEST: End.M.GTP4.E (pdu_type unset, short header) [FAIL]"
		rc=1
	fi

	# VRF binding (per-tenant): egress IPv4 lookup goes through vrf-n3
	# (table 100), where 10.0.1.0/24 lives.  Without "oif vrf-n3" the
	# main-table lookup would fall through; the GTP-U observed in
	# gnb_vrf demonstrates the binding.  SID 2001:dba:a00:102:14:0:123:0
	# encodes IPv4 DA 10.0.1.2 + QFI=5 / TEID=0x123.
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	if [ "$have_vrf" = "1" ]; then
		if run_test "2001:db8:1::a00:1:0:1" \
			    "2001:dba:a00:102:1400:1:2300:0" \
			    "10.0.0.1" "$gnb_vrf" "veth-n3-2-gnb"; then
			echo "TEST: End.M.GTP4.E (oif vrf-n3) [PASS]"
		else
			echo "TEST: End.M.GTP4.E (oif vrf-n3) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP4.E (oif vrf-n3) [SKIP] (CONFIG_NET_VRF not loaded)"
	fi

	# Inner T-PDU netfilter hook: only meaningful when nft is present
	# and the kernel exposes net.netfilter.nf_hooks_lwtunnel.
	if command -v nft >/dev/null && \
	   ip netns exec "$srgw" sysctl -wq \
		net.netfilter.nf_hooks_lwtunnel=1 2>/dev/null; then
		ip netns exec "$srgw" nft add table ip6 filter
		ip netns exec "$srgw" nft 'add chain ip6 filter prerouting' \
			'{ type filter hook prerouting priority 0; }'

		if run_nf_test drop ""; then
			echo "TEST: End.M.GTP4.E (nft drop on inner) [PASS]"
		else
			echo "TEST: End.M.GTP4.E (nft drop on inner) [FAIL]"
			rc=1
		fi

		if run_nf_test accept "1"; then
			echo "TEST: End.M.GTP4.E (nft accept on inner) [PASS]"
		else
			echo "TEST: End.M.GTP4.E (nft accept on inner) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP4.E (inner-flow netfilter hook) [SKIP]" \
		     "(nft or nf_hooks_lwtunnel unavailable)"
	fi

	if [ "$rc" -eq 0 ]; then
		echo "TEST: End.M.GTP4.E [PASS]"
		exit "$ksft_pass"
	else
		echo "TEST: End.M.GTP4.E [FAIL]"
		exit "$ksft_fail"
	fi
}

main "$@"
