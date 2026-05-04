#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2034,SC2154
#
# Selftest for the SRv6 H.M.GTP4.D behavior (RFC 9433 Section 6.7).
#
#   +-------+     10.0.0.0/24     +-------+   2001:db8:2::/64   +-------+
#   |  gnb  | ------------------- | srgw  | ------------------- | srupf |
#   +-------+        veth-n3      +-------+        veth-n9      +-------+
#                                     |
#                                     |        10.10.0.0/24
#                                     +--------veth-n6--------- +-------+
#                                                               | lupf  |
#                                                               +-------+
#
# gnb is the GTP-U-side test peer that injects the GTP-U packets.
# srupf is the SR-domain-side SRv6-aware UPF (RFC 9433 sense, not
# a 3GPP UPF) that receives the resulting SRv6 T-PDU.  lupf is the
# SRv6-non-aware legacy UPF that owns the GTP-U control plane and
# receives non-T-PDU GTP-U (Echo Request, Error Indication, ...)
# forwarded by srgw via the H.M.GTP4.D route's dev.  srgw runs the
# H.M.GTP4.D behavior under test.
#
# An H.M.GTP4.D SID is installed on the SR ingress for IPv4 destination
# 10.99.0.0/24 with v4_mask_len=32 and sr_prefix_len=32; Args.Mob.Session is
# the fixed 40-bit field defined by RFC 9433 Section 6.1, Figure 8.  The
# H.M.GTP4.D SID locator prefix is 2001:db8::, so an inbound IPv4/UDP/GTP-U
# packet to 10.99.0.2 with TEID 0x123 (and PDU Session ext carrying QFI=5) is
# expected to come out as IPv6 toward 2001:db8:a63:2:1400:1:2300:0,
# where:
#
#   bytes 0-3  (locator /32)        = 20 01 0d b8
#   bytes 4-7  (IPv4 DA, 32-bit)    = 0a 63 00 02   (= 10.99.0.2)
#   bytes 8-12 (Args.Mob.Session)   = 14 00 00 01 23
#                              (QFI byte 0x14 + 32-bit PDU/TEID 0x123)
#   bytes 13-15 (SID padding)       = 00 00 00

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
	setup_ns gnb srgw srupf lupf srupf_vrf

	ip -n "$gnb" link set lo up
	ip -n "$srgw" link set lo up
	ip -n "$srupf" link set lo up
	ip -n "$lupf" link set lo up
	ip -n "$srupf_vrf" link set lo up

	ip link add veth-n3 netns "$gnb" type veth peer name veth-n3-srgw \
		netns "$srgw"
	ip -n "$gnb" addr add 10.0.0.2/24 dev veth-n3
	ip -n "$srgw" addr add 10.0.0.1/24 dev veth-n3-srgw
	ip -n "$gnb" link set veth-n3 up
	ip -n "$srgw" link set veth-n3-srgw up

	ip link add veth-n9 netns "$srgw" type veth peer name veth-n9-srupf \
		netns "$srupf"
	ip -n "$srgw" addr add 2001:db8:2::1/64 dev veth-n9 nodad
	ip -n "$srupf" addr add 2001:db8:2::e/64 dev veth-n9-srupf nodad
	ip -n "$srgw" link set veth-n9 up
	ip -n "$srupf" link set veth-n9-srupf up

	# Legacy IPv4 UPF reachable from srgw; non-T-PDU GTP-U is forwarded
	# here via the H.M.GTP4.D route's dev so the legacy GTP-U control
	# plane (Echo Request / Response) can be answered downstream.
	ip link add veth-n6 netns "$srgw" type veth peer name veth-n6-lupf \
		netns "$lupf"
	ip -n "$srgw" addr add 10.10.0.1/24 dev veth-n6
	ip -n "$lupf" addr add 10.10.0.2/24 dev veth-n6-lupf
	ip -n "$srgw" link set veth-n6 up
	ip -n "$lupf" link set veth-n6-lupf up

	ip netns exec "$srgw" sysctl -wq net.ipv4.ip_forward=1
	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.all.forwarding=1

	ip -n "$gnb" route add 10.99.0.0/24 via 10.0.0.1

	# Install H.M.GTP4.D on an IPv4 route.  sr_prefix_len declares the
	# locator length used by the remote End.M.GTP4.E SID.  dev veth-n6
	# is the legacy UPF leg: T-PDU encap takes the IPv6 SR Policy path
	# (independent of dst.dev) while non-T-PDU is forwarded out veth-n6
	# via ip_forward.
	ip -n "$srgw" -4 route add 10.99.0.0/24 \
		encap seg6local action H.M.GTP4.D \
			nh6 2001:db8:: \
			src 2001:db8:2::1 \
			v4_mask_len 32 sr_prefix_len 32 \
		dev veth-n6

	# srgw needs to reach the constructed SID; the /32 prefix covers
	# any IPv4 DA + Args.Mob.Session combination derived from the
	# locator 2001:db8::.
	ip -n "$srgw" -6 route add 2001:db8::/32 \
		via 2001:db8:2::e dev veth-n9
	ip -n "$srupf" -6 route add 2001:db8::/32 dev veth-n9-srupf

	local upf_mac
	upf_mac=$(ip -n "$srupf" -j link show veth-n9-srupf | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')
	ip -n "$srgw" -6 neigh replace 2001:db8:2::e dev veth-n9 \
		lladdr "$upf_mac" nud permanent 2>/dev/null || true

	# Pre-resolve the IPv4 ARP entry for the SID-prefix DA so non-T-PDU
	# Echo can be forwarded to lupf without ARP delay.
	local lupf_mac
	lupf_mac=$(ip -n "$lupf" -j link show veth-n6-lupf | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')
	ip -n "$srgw" neigh replace 10.99.0.2 dev veth-n6 \
		lladdr "$lupf_mac" nud permanent 2>/dev/null || true

	# Per-route VRF case: a second SR-side upf in its own VRF.  The
	# H.M.GTP4.D SID for this tenant binds the SRv6 underlay output to
	# the VRF via 'oif'.  Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	modprobe vrf 2>/dev/null
	if ip -n "$srgw" link add vrf-n9 type vrf table 100 2>/dev/null; then
		have_vrf=1
		ip -n "$srgw" link set dev vrf-n9 up

		ip link add veth-n9-2 netns "$srgw" type veth peer name \
			veth-n9-2-srupf netns "$srupf_vrf"
		ip -n "$srgw" link set dev veth-n9-2 master vrf-n9
		ip -n "$srgw" addr add 2001:db8:4::1/64 dev veth-n9-2 nodad
		ip -n "$srupf_vrf" addr add 2001:db8:4::e/64 dev veth-n9-2-srupf \
			nodad
		ip -n "$srgw" link set dev veth-n9-2 up
		ip -n "$srupf_vrf" link set dev veth-n9-2-srupf up

		# H.M.GTP4.D for a second IPv4 prefix bound to vrf-n9; the
		# constructed SID's locator is 2001:db9::/32 (a separate locator
		# so the two routes never collide).
		ip -n "$srgw" -4 route add 10.99.1.0/24 \
			encap seg6local action H.M.GTP4.D \
				nh6 2001:db9:: \
				src 2001:db8:2::1 \
				v4_mask_len 32 sr_prefix_len 32 \
				oif vrf-n9 \
			dev veth-n9-2

		# Reach the constructed SID via the VRF table.
		ip -n "$srgw" -6 route add 2001:db9::/32 \
			via 2001:db8:4::e dev veth-n9-2 vrf vrf-n9
		ip -n "$srupf_vrf" -6 route add 2001:db9::/32 \
			dev veth-n9-2-srupf

		local upf_vrf_mac
		upf_vrf_mac=$(ip -n "$srupf_vrf" -j link show \
			veth-n9-2-srupf | python3 -c \
			'import sys, json; print(json.load(sys.stdin)[0]["address"])')
		ip -n "$srgw" -6 neigh replace 2001:db8:4::e dev veth-n9-2 \
			lladdr "$upf_vrf_mac" nud permanent 2>/dev/null || true

		ip -n "$gnb" route add 10.99.1.0/24 via 10.0.0.1
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
	if ! python3 -c "import scapy.all" 2>/dev/null; then
		echo "SKIP: python3-scapy is required"; exit "$ksft_skip"
	fi

	if ! ip route help 2>&1 | grep -qF "H.M.GTP4.D"; then
		echo "SKIP: iproute2 too old, missing seg6local action H.M.GTP4.D"
		exit "$ksft_skip"
	fi
}

send_gtpu()
{
	local v4_dst="$1"
	local srgw_mac

	srgw_mac=$(ip -n "$srgw" -j link show veth-n3-srgw | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')

	SRGW_MAC="$srgw_mac" V4_DST="$v4_dst" ip netns exec "$gnb" python3 - <<'PY'
import os
from scapy.all import IP, UDP, ICMP, sendp, Ether
mac = os.environ['SRGW_MAC']
v4_dst = os.environ['V4_DST']
gtpu = bytes.fromhex(
    "34 ff 00 24 00 00 01 23 00 00 00 85"
    "01 00 05 00")
inner = bytes(IP(src='10.0.0.2', dst=v4_dst) / ICMP())
pkt = (Ether(dst=mac) /
       IP(src='10.0.0.2', dst=v4_dst) /
       UDP(sport=2152, dport=2152) /
       (gtpu + inner))
sendp(pkt, iface='veth-n3', verbose=False)
PY
}

# Send a GTPv1-U Echo Request; H.M.GTP4.D must NOT consume it but
# pass it through to the configured forwarding path so the legacy UPF
# (which owns the GTP-U control plane) can answer.  Verified by
# capturing the unaltered Echo Request (type 0x01) on the lupf side.
send_gtpu_echo()
{
	local v4_dst="$1"
	local srgw_mac

	srgw_mac=$(ip -n "$srgw" -j link show veth-n3-srgw | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')

	SRGW_MAC="$srgw_mac" V4_DST="$v4_dst" ip netns exec "$gnb" python3 - <<'PY'
import os
from scapy.all import IP, UDP, sendp, Ether
mac = os.environ['SRGW_MAC']
v4_dst = os.environ['V4_DST']
gtpu_echo = bytes.fromhex("32 01 00 04 00 00 00 00 42 42 00 00")
pkt = (Ether(dst=mac) /
       IP(src='10.0.0.2', dst=v4_dst) /
       UDP(sport=2152, dport=2152) /
       gtpu_echo)
sendp(pkt, iface='veth-n3', verbose=False)
PY
}

run_echo_test()
{
	local v4_dst="$1"
	local out
	local rc

	out=$(mktemp)

	ip netns exec "$lupf" tcpdump -U -nni veth-n6-lupf -w "$out" \
		'udp port 2152' 2>/dev/null &
	tcpdump_pid=$!
	sleep 1

	send_gtpu_echo "$v4_dst"

	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""

	V4_DST="$v4_dst" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IP, UDP

want_dst = os.environ['V4_DST']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IP not in p or UDP not in p:
        continue
    if p[UDP].sport != 2152 or p[UDP].dport != 2152:
        continue
    if p[IP].dst != want_dst:
        continue
    payload = bytes(p[UDP].payload)
    if len(payload) >= 2 and payload[1] == 0x01:
        sys.exit(0)
sys.exit("no GTPv1-U Echo Request observed at lupf "
         "(H.M.GTP4.D failed to pass non-T-PDU through)")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

capture_traffic()
{
	local capture_ns="$1"
	local capture_iface="$2"
	local v4_dst="$3"
	local out="$4"

	ip netns exec "$capture_ns" tcpdump -U -nni "$capture_iface" -w "$out" \
		'ip6' 2>/dev/null &
	tcpdump_pid=$!
	# Give tcpdump a brief moment to attach the BPF filter.
	sleep 1

	send_gtpu "$v4_dst"

	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""
}

run_test()
{
	local v4_dst="$1"		# inner IPv4 DA fed into the gNB
	local locator_octets="$2"	# "20 01 0d b8"
	local v4_dst_octets="$3"	# "0a 63 00 02" (10.99.0.2) etc
	local sa_pos="$4"		# byte offset of expected IPv4 SA in IPv6 SA
	local capture_ns="${5:-$srupf}"
	local capture_iface="${6:-veth-n9-srupf}"
	local out
	local rc

	out=$(mktemp)
	capture_traffic "$capture_ns" "$capture_iface" "$v4_dst" "$out"

	# scapy field check: an IPv6 packet must reach upf with:
	# - DST address whose bytes 0..3 = locator, bytes 4..7 = original
	#   IPv4 DA, bytes 8..12 = 40-bit Args.Mob.Session
	#   (0x14 = QFI=5, then TEID 0x00000123), bytes 13..15 = padding.
	# - SRC address whose bytes [sa_pos..sa_pos+4) = original IPv4 SA
	#   (10.0.0.2) per RFC 9433 Section 6.6 Figure 10.
	LOC="$locator_octets" V4="$v4_dst_octets" SA_POS="$sa_pos" \
	python3 - "$out" <<'PYEOF'
import ipaddress
import os
import sys
from scapy.all import rdpcap, IPv6

loc = bytes.fromhex(os.environ['LOC'])
v4_dst = bytes.fromhex(os.environ['V4'])
sa_pos = int(os.environ['SA_POS'])
expected_v4_sa = bytes.fromhex('0a 00 00 02')

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 not in p:
        continue
    da = ipaddress.IPv6Address(str(p[IPv6].dst)).packed
    sa = ipaddress.IPv6Address(str(p[IPv6].src)).packed
    if da[0:4] != loc:
        continue
    if da[4:8] != v4_dst:
        sys.exit(f"unexpected SID v4-DA slice {da[4:8].hex()}, want {v4_dst.hex()}")
    if da[8:13] != bytes.fromhex("1400000123"):
        sys.exit(f"unexpected Args.Mob.Session {da[8:13].hex()}")
    if sa[sa_pos:sa_pos + 4] != expected_v4_sa:
        sys.exit(f"unexpected IPv4 SA at byte {sa_pos}: "
                 f"{sa[sa_pos:sa_pos + 4].hex()}, want {expected_v4_sa.hex()}")
    sys.exit(0)
sys.exit("no IPv6 packet matching the expected SID locator")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

# Verify that nf_hooks_lwtunnel=1 makes the inner T-PDU 5-tuple
# visible to nftables on the SR Gateway.  The inner is IPv4
# (10.0.0.2 -> v4_dst, set by send_gtpu()); the nft rule matches on
# the inner IPv4 source.  DROP must suppress the SRv6 packet at the
# upf, ACCEPT must let it through.
run_nf_test()
{
	local verdict="$1"		# drop | accept
	local expect="$2"		# 1 if SRv6 expected, empty otherwise
	local v4_dst="10.99.0.2"
	local out

	ip netns exec "$srgw" nft flush chain ip filter prerouting
	ip netns exec "$srgw" nft add rule ip filter prerouting \
		ip saddr 10.0.0.2 "$verdict"

	out=$(mktemp)
	capture_traffic "$srupf" "veth-n9-srupf" "$v4_dst" "$out"

	if [ -n "$expect" ]; then
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IPv6

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 in p:
        sys.exit(0)
sys.exit("expected SRv6 packet not observed at upf despite nft accept")
PYEOF
	else
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IPv6

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 in p and bytes(p[IPv6])[6] == 0x29:
        # nexthdr == IPIP (41) means an SRv6-encapped IPIP packet
        sys.exit("SRv6 packet leaked to upf despite nft drop on inner")
    if IPv6 in p and bytes(p[IPv6])[6] == 0x2b:
        # nexthdr == 43 (Routing) means SRH present
        sys.exit("SRv6 packet leaked to upf despite nft drop on inner")
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

	# Hard-coded /64 layout: IPv4 SA at IPv6 bytes 8..11.
	if run_test "10.99.0.2" "20 01 0d b8" "0a 63 00 02" 8; then
		echo "TEST: H.M.GTP4.D (default) [PASS]"
	else
		echo "TEST: H.M.GTP4.D (default) [FAIL]"
		rc=1
	fi

	if run_echo_test "10.99.0.2"; then
		echo "TEST: H.M.GTP4.D (non-T-PDU passthrough) [PASS]"
	else
		echo "TEST: H.M.GTP4.D (non-T-PDU passthrough) [FAIL]"
		rc=1
	fi

	# VRF binding: SRv6 underlay output goes through vrf-n9 (table 100).
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	if [ "$have_vrf" = "1" ]; then
		# Locator 2001:db9::/32 -> "20 01 0d b9", v4 dst 10.99.1.2 ->
		# "0a 63 01 02".
		if run_test "10.99.1.2" "20 01 0d b9" "0a 63 01 02" 8 \
			    "$srupf_vrf" "veth-n9-2-srupf"; then
			echo "TEST: H.M.GTP4.D (oif vrf-n9) [PASS]"
		else
			echo "TEST: H.M.GTP4.D (oif vrf-n9) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: H.M.GTP4.D (oif vrf-n9) [SKIP] (CONFIG_NET_VRF not loaded)"
	fi

	# Inner T-PDU netfilter hook: only meaningful when nft is present
	# and the kernel exposes net.netfilter.nf_hooks_lwtunnel.
	if command -v nft >/dev/null && \
	   ip netns exec "$srgw" sysctl -wq \
		net.netfilter.nf_hooks_lwtunnel=1 2>/dev/null; then
		ip netns exec "$srgw" nft add table ip filter
		ip netns exec "$srgw" nft 'add chain ip filter prerouting' \
			'{ type filter hook prerouting priority 0; }'

		if run_nf_test drop ""; then
			echo "TEST: H.M.GTP4.D (nft drop on inner) [PASS]"
		else
			echo "TEST: H.M.GTP4.D (nft drop on inner) [FAIL]"
			rc=1
		fi

		if run_nf_test accept "1"; then
			echo "TEST: H.M.GTP4.D (nft accept on inner) [PASS]"
		else
			echo "TEST: H.M.GTP4.D (nft accept on inner) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: H.M.GTP4.D (inner-flow netfilter hook) [SKIP]" \
		     "(nft or nf_hooks_lwtunnel unavailable)"
	fi

	if [ "$rc" -eq 0 ]; then
		echo "TEST: H.M.GTP4.D [PASS]"
		exit "$ksft_pass"
	else
		echo "TEST: H.M.GTP4.D [FAIL]"
		exit "$ksft_fail"
	fi
}

main "$@"
