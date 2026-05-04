#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2034,SC2154
#
# Selftest for the SRv6 End.M.GTP6.E behavior (RFC 9433 Section 6.5).
#
#   +-------+   2001:db8:1::/64   +-------+   2001:db8:2::/64   +-------+
#   | srupf | ------------------- | srgw  | ------------------- |  gnb  |
#   +-------+        veth-n9      +-------+        veth-n3      +-------+
#
# srupf is the SR-domain-side SRv6-aware UPF (RFC 9433 sense, not a
# 3GPP UPF) that injects the SRv6 packets, gnb is the GTP-U-side
# test peer, and srgw runs the End.M.GTP6.E behavior under test.
#
# An End.M.GTP6.E SID is installed on srgw for locator
# 2001:db8:f::/64 with src=2001:db8:2::1.  Args.Mob.Session is the
# fixed 40-bit field defined by RFC 9433 Section 6.1, Figure 8, immediately
# after the locator (here at byte offset 8).  The bytes after
# Args.Mob.Session are SID padding and are ignored by the egress.
# The srupf uses scapy to inject an SRv6 packet with:
#
#   outer DA               = 2001:db8:f::1400:1:2300:0
#                            (locator 2001:db8:f::/64 followed by
#                             Args.Mob.Session bytes 14 00 00 01 23 at
#                             offset 8, which encode QFI=5 and
#                             PDU Session ID=0x123, plus 24 bits of
#                             SID padding)
#   SRH segments[0]        = 2001:db8:2::2  (gNB, next destination)
#   SRH segments[1]        = 2001:db8:f::1400:1:2300:0 (current SID)
#   SRH segments_left      = 1
#
# The expected output on veth-n3-gnb is an IPv6/UDP/GTP-U(long)/PDU-Session-ext
# packet toward 2001:db8:2::2 carrying TEID 0x00000123 and QFI 5.

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

	ip -n "$srupf" link set lo up
	ip -n "$srgw" link set lo up
	ip -n "$gnb" link set lo up
	ip -n "$gnb_vrf" link set lo up

	ip link add veth-n9 netns "$srupf" type veth peer name veth-n9-srgw \
		netns "$srgw"
	ip -n "$srupf" addr add 2001:db8:1::1/64 dev veth-n9 nodad
	ip -n "$srgw" addr add 2001:db8:1::2/64 dev veth-n9-srgw nodad
	ip -n "$srupf" link set veth-n9 up
	ip -n "$srgw" link set veth-n9-srgw up

	ip link add veth-n3 netns "$srgw" type veth peer name veth-n3-gnb \
		netns "$gnb"
	ip -n "$srgw" addr add 2001:db8:2::1/64 dev veth-n3 nodad
	ip -n "$gnb" addr add 2001:db8:2::2/64 dev veth-n3-gnb nodad
	ip -n "$srgw" link set veth-n3 up
	ip -n "$gnb" link set veth-n3-gnb up

	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.all.forwarding=1

	# install End.M.GTP6.E on srgw with PDU Session Container (5G N3:
	# pdu_type dl), /64 locator.
	ip -n "$srgw" -6 route add 2001:db8:f::/64 \
		encap seg6local action End.M.GTP6.E \
			src 2001:db8:2::1 pdu_type dl \
		dev veth-n3

	# install End.M.GTP6.E on srgw WITHOUT pdu_type: short GTPv1-U
	# (LTE-style, no PDU Session Container) regardless of QFI.
	ip -n "$srgw" -6 route add 2001:db8:fa::/64 \
		encap seg6local action End.M.GTP6.E \
			src 2001:db8:2::1 \
		dev veth-n3

	# Per-route VRF case: a second egress IPv6 path in its own VRF so we
	# can verify that the End.M.GTP6.E SID's egress GTP-U lookup uses
	# the configured 'oif' rather than the main routing table.
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	modprobe vrf 2>/dev/null
	if ip -n "$srgw" link add vrf-n3 type vrf table 100 2>/dev/null; then
		have_vrf=1
		ip -n "$srgw" link set dev vrf-n3 up

		ip link add veth-n3-2 netns "$srgw" type veth peer name \
			veth-n3-2-gnb netns "$gnb_vrf"
		ip -n "$srgw" link set dev veth-n3-2 master vrf-n3
		ip -n "$srgw" addr add 2001:db8:3::1/64 dev veth-n3-2 nodad
		ip -n "$gnb_vrf" addr add 2001:db8:3::2/64 dev veth-n3-2-gnb nodad
		ip -n "$srgw" link set dev veth-n3-2 up
		ip -n "$gnb_vrf" link set dev veth-n3-2-gnb up

		ip -n "$srgw" -6 route add 2001:db8:e::/64 \
			encap seg6local action End.M.GTP6.E \
				src 2001:db8:3::1 oif vrf-n3 pdu_type dl \
			dev veth-n3-2
	fi
}

check_dependencies()
{
	if ! command -v tcpdump >/dev/null; then
		echo "SKIP: tcpdump is required"; exit "$ksft_skip"
	fi
	if ! command -v python3 >/dev/null; then
		echo "SKIP: python3 is required"; exit "$ksft_skip"
	fi
	if ! python3 -c "from scapy.layers.inet6 import IPv6ExtHdrSegmentRouting" 2>/dev/null; then
		echo "SKIP: python3-scapy with SRv6 support is required"
		exit "$ksft_skip"
	fi

	if ! ip route help 2>&1 | grep -qF "End.M.GTP6.E"; then
		echo "SKIP: iproute2 too old, missing seg6local action End.M.GTP6.E"
		exit "$ksft_skip"
	fi
}

inject_srv6()
{
	local sid="$1"		# outer IPv6 DA (current End.M.GTP6.E SID)
	local next_seg="$2"	# SRH segments[0] (next destination = gNB)
	local srgw_mac

	srgw_mac=$(ip -n "$srgw" -j link show veth-n9-srgw | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')

	SRGW_MAC="$srgw_mac" SID="$sid" NEXT_SEG="$next_seg" \
		ip netns exec "$srupf" python3 - <<'PY'
import os
from scapy.all import IPv6, ICMPv6EchoRequest, sendp, Ether
from scapy.layers.inet6 import IPv6ExtHdrSegmentRouting

mac = os.environ['SRGW_MAC']
sid = os.environ['SID']
next_seg = os.environ['NEXT_SEG']
inner = IPv6(src='2001:db8:1::1', dst='2001:db8:dead::1') / \
        ICMPv6EchoRequest(data=b'X' * 16)
srh = IPv6ExtHdrSegmentRouting(
    addresses=[next_seg, sid],
    segleft=1, lastentry=1, nh=41)
pkt = Ether(dst=mac) / \
      IPv6(src='2001:db8:1::1', dst=sid, nh=43) / \
      srh / inner
sendp(pkt, iface='veth-n9', verbose=False)
PY
}

capture_traffic()
{
	local capture_ns="$1"
	local capture_iface="$2"
	local sid="$3"
	local next_seg="$4"
	local out="$5"

	ip netns exec "$capture_ns" tcpdump -U -nni "$capture_iface" -w "$out" \
		'ip6 and udp port 2152' 2>/dev/null &
	tcpdump_pid=$!
	# Give tcpdump a brief moment to attach the BPF filter.
	sleep 1

	inject_srv6 "$sid" "$next_seg"

	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""
}

run_test()
{
	local sid="$1"			# End.M.GTP6.E SID to send to
	local next_seg="$2"		# expected outer IPv6 DA in egress GTP-U
	local capture_ns="${3:-$gnb}"	# netns where GTP-U is expected to land
	local capture_iface="${4:-veth-n3-gnb}"
	local out

	out=$(mktemp)
	capture_traffic "$capture_ns" "$capture_iface" "$sid" "$next_seg" "$out"

	# Verify with scapy field comparison: the captured frame must be
	# IPv6/UDP(2152)/GTP-U toward $next_seg, carry TEID 0x00000123 and a
	# PDU Session ext with QFI=5.
	NEXT_SEG="$next_seg" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IPv6, UDP

next_seg = os.environ['NEXT_SEG']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if not (IPv6 in p and UDP in p):
        continue
    if str(p[IPv6].dst) != next_seg:
        continue
    if p[UDP].dport != 2152:
        continue
    payload = bytes(p[UDP].payload)
    if len(payload) < 12:
        continue
    teid = int.from_bytes(payload[4:8], 'big')
    if teid != 0x00000123:
        sys.exit(f"unexpected TEID 0x{teid:08x}, want 0x00000123")
    if payload[11] != 0x85:
        sys.exit(f"missing PDU Session ext (next={payload[11]:#04x}, want 0x85)")
    pdu_session = payload[12:16]
    if pdu_session[0] != 0x01 or (pdu_session[2] & 0x3f) != 5:
        sys.exit(f"PDU Session fields unexpected: {pdu_session.hex()} (want 01 ?? 05 00)")
    sys.exit(0)
sys.exit(f"no IPv6/UDP/GTP-U packet observed toward {next_seg}")
PYEOF
	local rc=$?
	rm -f "$out"
	return $rc
}

# Verify the short-GTPv1-U output produced when pdu_type is unset on the
# route: 8-byte GTP-U header, no extension flag, no PDU Session
# Container, regardless of the QFI extracted from Args.Mob.Session.
run_test_short()
{
	local sid="$1"
	local next_seg="$2"
	local out
	local rc

	out=$(mktemp)
	capture_traffic "$gnb" "veth-n3-gnb" "$sid" "$next_seg" "$out"

	NEXT_SEG="$next_seg" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IPv6, UDP

next_seg = os.environ['NEXT_SEG']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if not (IPv6 in p and UDP in p):
        continue
    if str(p[IPv6].dst) != next_seg:
        continue
    if p[UDP].dport != 2152:
        continue
    payload = bytes(p[UDP].payload)
    if len(payload) < 8:
        continue
    flags = payload[0]
    if flags != 0x30:
        sys.exit(f"unexpected GTP-U flags {flags:#04x}, want 0x30 (short)")
    teid = int.from_bytes(payload[4:8], 'big')
    if teid != 0x00000123:
        sys.exit(f"unexpected TEID 0x{teid:08x}, want 0x00000123")
    sys.exit(0)
sys.exit(f"no IPv6/UDP/GTP-U packet observed toward {next_seg}")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

# Verify that nf_hooks_lwtunnel=1 makes the inner T-PDU 5-tuple
# visible to nftables on the SR Gateway.  The inner is IPv6
# (2001:db8:1::1 -> 2001:db8:dead::1, set by inject_srv6()); the nft
# rule matches on its IPv6 source address.  DROP must suppress the
# GTP-U at the gnb, ACCEPT must let it through.
run_nf_test()
{
	local verdict="$1"		# drop | accept
	local expect="$2"		# 1 if GTP-U expected, empty otherwise
	local sid="2001:db8:f::1400:1:2300:0"
	local next_seg="2001:db8:2::2"
	local out

	ip netns exec "$srgw" nft flush chain ip6 filter prerouting
	ip netns exec "$srgw" nft add rule ip6 filter prerouting \
		ip6 saddr 2001:db8:1::1 "$verdict"

	out=$(mktemp)
	capture_traffic "$gnb" "veth-n3-gnb" "$sid" "$next_seg" "$out"

	if [ -n "$expect" ]; then
		NEXT_SEG="$next_seg" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IPv6, UDP

next_seg = os.environ['NEXT_SEG']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 in p and UDP in p and \
       str(p[IPv6].dst) == next_seg and p[UDP].dport == 2152:
        sys.exit(0)
sys.exit("expected GTP-U packet not observed at gnb despite nft accept")
PYEOF
	else
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IPv6, UDP

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 in p and UDP in p and p[UDP].dport == 2152:
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

	if run_test "2001:db8:f::1400:1:2300:0" "2001:db8:2::2"; then
		echo "TEST: End.M.GTP6.E (default) [PASS]"
	else
		echo "TEST: End.M.GTP6.E (default) [FAIL]"
		rc=1
	fi

	# pdu_type unset: emit short GTPv1-U with no PDU Session Container
	# even though Args.Mob.Session encodes QFI=5.
	if run_test_short "2001:db8:fa::1400:1:2300:0" "2001:db8:2::2"; then
		echo "TEST: End.M.GTP6.E (pdu_type unset, short header) [PASS]"
	else
		echo "TEST: End.M.GTP6.E (pdu_type unset, short header) [FAIL]"
		rc=1
	fi

	# VRF binding: egress IPv6 GTP-U goes through vrf-n3 (table 100),
	# where the route to 2001:db8:3::/64 lives.  Without "oif vrf-n3"
	# the main-table lookup would fall through; the GTP-U observed in
	# gnb_vrf demonstrates the binding.
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	if [ "$have_vrf" = "1" ]; then
		if run_test "2001:db8:e::1400:1:2300:0" "2001:db8:3::2" \
			    "$gnb_vrf" "veth-n3-2-gnb"; then
			echo "TEST: End.M.GTP6.E (oif vrf-n3) [PASS]"
		else
			echo "TEST: End.M.GTP6.E (oif vrf-n3) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP6.E (oif vrf-n3) [SKIP] (CONFIG_NET_VRF not loaded)"
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
			echo "TEST: End.M.GTP6.E (nft drop on inner) [PASS]"
		else
			echo "TEST: End.M.GTP6.E (nft drop on inner) [FAIL]"
			rc=1
		fi

		if run_nf_test accept "1"; then
			echo "TEST: End.M.GTP6.E (nft accept on inner) [PASS]"
		else
			echo "TEST: End.M.GTP6.E (nft accept on inner) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP6.E (inner-flow netfilter hook) [SKIP]" \
		     "(nft or nf_hooks_lwtunnel unavailable)"
	fi

	if [ "$rc" -eq 0 ]; then
		echo "TEST: End.M.GTP6.E [PASS]"
		exit "$ksft_pass"
	else
		echo "TEST: End.M.GTP6.E [FAIL]"
		exit "$ksft_fail"
	fi
}

main "$@"
