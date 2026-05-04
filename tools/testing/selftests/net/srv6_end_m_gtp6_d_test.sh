#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# shellcheck disable=SC2034,SC2154
#
# Selftest for the SRv6 End.M.GTP6.D behavior (RFC 9433 Section 6.3).
#
#   +-------+   2001:db8:1::/64   +-------+   2001:db8:2::/64   +-------+
#   |  gnb  | ------------------- | srgw  | ------------------- | srupf |
#   +-------+        veth-n3      +-------+        veth-n9      +-------+
#                                     |
#                                     |   2001:db8:6::/64
#                                     +--------veth-n6--------- +-------+
#                                                               | lupf  |
#                                                               +-------+
#
# gnb is the GTP-U-side test peer that injects the GTP-U packets.
# srupf is the SR-domain-side SRv6-aware UPF (RFC 9433 sense, not
# a 3GPP UPF) that receives the resulting SRv6 T-PDU.  lupf is the
# SRv6-non-aware legacy UPF that owns the GTP-U control plane and
# receives non-T-PDU GTP-U (Echo Request, Error Indication, ...)
# forwarded by srgw via the H.M.GTP6.D route's dev.  srgw runs the
# End.M.GTP6.D behavior under test.
#
# An End.M.GTP6.D SID is installed on srgw for locator
# 2001:db8:f::/48 with src=2001:db8:2::1.  Args.Mob.Session is the
# fixed 40-bit field defined by RFC 9433 Section 6.1, Figure 8.  When gnb sends an
# IPv6/UDP/GTP-U packet to 2001:db8:f::1 carrying TEID 0x123, the srgw
# is expected to emit an SRv6 packet toward 2001:db8:3::e whose last
# SRH segment carries Args.Mob.Session in its right-aligned 40-bit
# tail (QFI=5, R=0, U=0, PDU Session ID=0x123 → bytes 14 00 00 01 23).

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

	# gnb <-> srgw
	ip link add veth-n3 netns "$gnb" type veth peer name veth-n3-srgw \
		netns "$srgw"
	ip -n "$gnb" addr add 2001:db8:1::2/64 dev veth-n3 nodad
	ip -n "$srgw" addr add 2001:db8:1::1/64 dev veth-n3-srgw nodad
	ip -n "$gnb" link set veth-n3 up
	ip -n "$srgw" link set veth-n3-srgw up

	# srgw <-> srupf  (SR-aware UPF, T-PDU SRv6 destination)
	ip link add veth-n9 netns "$srgw" type veth peer name veth-n9-srupf \
		netns "$srupf"
	ip -n "$srgw" addr add 2001:db8:2::1/64 dev veth-n9 nodad
	ip -n "$srupf" addr add 2001:db8:2::e/64 dev veth-n9-srupf nodad
	ip -n "$srgw" link set veth-n9 up
	ip -n "$srupf" link set veth-n9-srupf up

	# srgw <-> lupf  (legacy UPF, GTP-U control plane recipient)
	ip link add veth-n6 netns "$srgw" type veth peer name veth-n6-lupf \
		netns "$lupf"
	ip -n "$srgw" addr add 2001:db8:6::1/64 dev veth-n6 nodad
	ip -n "$lupf" addr add 2001:db8:6::e/64 dev veth-n6-lupf nodad
	ip -n "$srgw" link set veth-n6 up
	ip -n "$lupf" link set veth-n6-lupf up

	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.all.forwarding=1
	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.all.seg6_enabled=1
	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.veth-n9.seg6_enabled=1
	ip netns exec "$srgw" sysctl -wq net.ipv6.conf.veth-n6.seg6_enabled=1
	ip netns exec "$srupf" sysctl -wq net.ipv6.conf.all.seg6_enabled=1
	ip netns exec "$srupf" sysctl -wq net.ipv6.conf.veth-n9-srupf.seg6_enabled=1

	# route on gnb toward the End.M.GTP6.D SID
	ip -n "$gnb" -6 route add 2001:db8:f::/64 via 2001:db8:1::1

	# install End.M.GTP6.D on srgw.  sr_prefix_len declares the locator
	# length used by the remote End.M.GTP6.E SID; with /64 the kernel
	# writes Args.Mob.Session into bytes 8..12 of the penultimate SID.
	# dev veth-n6 is the legacy UPF leg: T-PDU encap takes the IPv6 SR
	# Policy path (independent of dst.dev) while non-T-PDU is forwarded
	# out veth-n6 via ip6_forward.
	ip -n "$srgw" -6 route add 2001:db8:f::/64 \
		encap seg6local action End.M.GTP6.D \
			srh segs 2001:db8:2::e,2001:db8:3::e \
			src 2001:db8:2::1 sr_prefix_len 64 count \
		dev veth-n6

	# accept the SRv6 packet on srupf
	ip -n "$srupf" -6 route add 2001:db8:2::e/128 dev lo
	ip -n "$srupf" -6 route add 2001:db8:3::/64 dev veth-n9-srupf

	# avoid ND-resolution timing flakiness with static neighbours
	local srupf_mac srgw_n9_mac lupf_mac
	srupf_mac=$(ip -n "$srupf" -j link show veth-n9-srupf | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')
	srgw_n9_mac=$(ip -n "$srgw" -j link show veth-n9 | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')
	lupf_mac=$(ip -n "$lupf" -j link show veth-n6-lupf | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')
	ip -n "$srgw" -6 neigh replace 2001:db8:2::e dev veth-n9 \
		lladdr "$srupf_mac" nud permanent
	ip -n "$srupf" -6 neigh replace 2001:db8:2::1 dev veth-n9-srupf \
		lladdr "$srgw_n9_mac" nud permanent
	# Non-T-PDU passthrough: srgw forwards GTP-U control out the
	# H.M.GTP6.D route's dev (veth-n6); pre-resolve the lupf neighbour
	# for the Echo Request DA.
	ip -n "$srgw" -6 neigh replace 2001:db8:f::1 dev veth-n6 \
		lladdr "$lupf_mac" nud permanent

	# Per-route VRF case: a second SR-side upf in its own VRF.  The
	# End.M.GTP6.D SID for this tenant binds the SRv6 underlay output to
	# the VRF via 'oif'; without it the lookup would fall through to
	# the main table.  Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
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

		ip netns exec "$srgw" sysctl -wq \
			net.ipv6.conf.veth-n9-2.seg6_enabled=1
		ip netns exec "$srupf_vrf" sysctl -wq \
			net.ipv6.conf.all.seg6_enabled=1
		ip netns exec "$srupf_vrf" sysctl -wq \
			net.ipv6.conf.veth-n9-2-srupf.seg6_enabled=1

		ip -n "$gnb" -6 route add 2001:db8:f0::/64 via 2001:db8:1::1

		ip -n "$srgw" -6 route add 2001:db8:f0::/64 \
			encap seg6local action End.M.GTP6.D \
				srh segs 2001:db8:4::e,2001:db8:5::e \
				src 2001:db8:4::1 sr_prefix_len 64 count \
				oif vrf-n9 \
			dev veth-n9-2

		ip -n "$srupf_vrf" -6 route add 2001:db8:4::e/128 dev lo
		ip -n "$srupf_vrf" -6 route add 2001:db8:5::/64 dev veth-n9-2-srupf

		local upf_vrf_mac
		local srgw_e2_mac
		upf_vrf_mac=$(ip -n "$srupf_vrf" -j link show \
			veth-n9-2-srupf | python3 -c \
			'import sys, json; print(json.load(sys.stdin)[0]["address"])')
		srgw_e2_mac=$(ip -n "$srgw" -j link show veth-n9-2 | \
			python3 -c \
			'import sys, json; print(json.load(sys.stdin)[0]["address"])')
		ip -n "$srgw" -6 neigh replace 2001:db8:4::e dev veth-n9-2 \
			lladdr "$upf_vrf_mac" nud permanent
		ip -n "$srupf_vrf" -6 neigh replace 2001:db8:4::1 \
			dev veth-n9-2-srupf lladdr "$srgw_e2_mac" nud permanent
	fi
}

check_dependencies()
{
	if ! command -v tcpdump >/dev/null; then
		echo "SKIP: tcpdump is required"
		exit "$ksft_skip"
	fi

	if ! command -v python3 >/dev/null; then
		echo "SKIP: python3 is required"
		exit "$ksft_skip"
	fi

	if ! ip route help 2>&1 | grep -qF "End.M.GTP6.D"; then
		echo "SKIP: iproute2 too old, missing seg6local action End.M.GTP6.D"
		exit "$ksft_skip"
	fi

	if ! python3 -c "import scapy.all" 2>/dev/null; then
		echo "SKIP: python3-scapy is required"
		exit "$ksft_skip"
	fi
}

send_gtpu()
{
	local outer_dst="$1"	# IPv6 destination of the GTP-U packet
	local srgw_mac

	srgw_mac=$(ip -n "$srgw" -j link show veth-n3-srgw | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')

	SRGW_MAC="$srgw_mac" OUTER_DST="$outer_dst" \
		ip netns exec "$gnb" python3 - <<'PY'
import os
from scapy.all import IPv6, UDP, IP, ICMP, sendp, Ether

mac = os.environ['SRGW_MAC']
outer_dst = os.environ['OUTER_DST']
# GTPv1 long header (E bit set, next ext = 0x85 PDU Session) carrying
# TEID 0x00000123, followed by a PDU Session ext (PDU Type=DL, QFI=5).
gtpu = bytes.fromhex(
    "34 ff 00 24 00 00 01 23 00 00 00 85"  # long header
    "01 00 05 00"                            # PDU Session ext
)
inner = bytes(IP(src="10.0.0.1", dst="10.0.0.2") / ICMP())
pkt = (Ether(dst=mac) /
       IPv6(src="2001:db8:1::2", dst=outer_dst) /
       UDP(sport=2152, dport=2152) /
       (gtpu + inner))
sendp(pkt, iface="veth-n3", verbose=False)
PY
}

# Send a GTPv1-U Echo Request; End.M.GTP6.D must NOT consume it but
# pass it through to the configured forwarding path so the downstream
# UPF (legacy GTP-U control plane) can answer.  Verified by capturing
# the unaltered Echo Request (type 0x01) on the upf side.
send_gtpu_echo()
{
	local outer_dst="$1"
	local srgw_mac

	srgw_mac=$(ip -n "$srgw" -j link show veth-n3-srgw | \
		python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["address"])')

	SRGW_MAC="$srgw_mac" OUTER_DST="$outer_dst" \
		ip netns exec "$gnb" python3 - <<'PY'
import os
from scapy.all import IPv6, UDP, sendp, Ether
mac = os.environ['SRGW_MAC']
outer_dst = os.environ['OUTER_DST']
gtpu_echo = bytes.fromhex("32 01 00 04 00 00 00 00 42 42 00 00")
pkt = (Ether(dst=mac) /
       IPv6(src="2001:db8:1::2", dst=outer_dst) /
       UDP(sport=2152, dport=2152) /
       gtpu_echo)
sendp(pkt, iface="veth-n3", verbose=False)
PY
}

run_echo_test()
{
	local outer_dst="$1"
	local out
	local rc

	out=$(mktemp)

	ip netns exec "$lupf" tcpdump -U -nni veth-n6-lupf -w "$out" \
		'udp port 2152' 2>/dev/null &
	tcpdump_pid=$!
	sleep 1

	send_gtpu_echo "$outer_dst"

	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""

	OUTER_DST="$outer_dst" python3 - "$out" <<'PYEOF'
import os, sys
from scapy.all import rdpcap, IPv6, UDP

want_dst = os.environ['OUTER_DST']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 not in p or UDP not in p:
        continue
    if p[UDP].sport != 2152 or p[UDP].dport != 2152:
        continue
    if p[IPv6].dst != want_dst:
        continue
    payload = bytes(p[UDP].payload)
    if len(payload) >= 2 and payload[1] == 0x01:
        sys.exit(0)
sys.exit("no GTPv1-U Echo Request observed at lupf "
         "(End.M.GTP6.D failed to pass non-T-PDU through)")
PYEOF
	rc=$?
	rm -f "$out"
	return $rc
}

capture_traffic()
{
	local capture_ns="$1"
	local capture_iface="$2"
	local outer_dst="$3"
	local out="$4"

	ip netns exec "$capture_ns" tcpdump -U -nni "$capture_iface" -w "$out" \
		'ip6' 2>/dev/null &
	tcpdump_pid=$!
	# Give tcpdump a brief moment to attach the BPF filter.
	sleep 1

	send_gtpu "$outer_dst"

	sleep 1
	kill -INT "$tcpdump_pid" 2>/dev/null
	wait "$tcpdump_pid" 2>/dev/null
	tcpdump_pid=""
}

run_test()
{
	local outer_dst="$1"			# GTP-U outer IPv6 DA
	local expected_srh0="$2"		# expected SRH[0] in upf
	local capture_ns="${3:-$srupf}"	# netns where SRv6 should land
	local capture_iface="${4:-veth-n9-srupf}"
	local out

	out=$(mktemp)
	capture_traffic "$capture_ns" "$capture_iface" "$outer_dst" "$out"

	# Verify with scapy: an SRv6 packet (IPv6 + Routing Header type 4)
	# must reach the upf.  Per RFC 9433 Section 6.5 Note, SRH[1]
	# carries Args.Mob.Session and SRH[0] carries the original outer DA.
	EXPECTED_SRH0="$expected_srh0" python3 - "$out" <<'PYEOF'
import ipaddress, os, sys
from scapy.all import rdpcap, IPv6, IPv6ExtHdrSegmentRouting

expected_srh0 = os.environ['EXPECTED_SRH0']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if not (IPv6 in p and IPv6ExtHdrSegmentRouting in p):
        continue
    srh = p[IPv6ExtHdrSegmentRouting]
    if srh.type != 4:
        sys.exit(f"unexpected RH type {srh.type}")
    if len(srh.addresses) < 2:
        continue
    srh0 = ipaddress.IPv6Address(str(srh.addresses[0])).packed
    if srh0 != ipaddress.IPv6Address(expected_srh0).packed:
        sys.exit(f"SRH[0] = {ipaddress.IPv6Address(srh0)} "
                 f"(want {expected_srh0}, the preserved outer DA)")
    srh1 = ipaddress.IPv6Address(str(srh.addresses[1])).packed
    args = srh1[8:13]
    if args != bytes.fromhex("1400000123"):
        sys.exit(f"Args.Mob.Session = {args.hex()} (want 1400000123)")
    sys.exit(0)
sys.exit("no SRv6 (RT6 type=4) packet with 2+ segments observed at upf")
PYEOF
	local rc=$?
	rm -f "$out"
	return $rc
}

# Verify that nf_hooks_lwtunnel=1 makes the inner T-PDU 5-tuple
# visible to nftables on the SR Gateway.  The nft rule matches on the
# inner IPv4 source address (10.0.0.1, set by send_gtpu()); a DROP
# verdict must prevent any SRv6 packet from reaching the upf, an
# ACCEPT verdict must let it through unchanged.
run_nf_test()
{
	local verdict="$1"		# drop | accept
	local expect_srh0="$2"	# preserved-DA test, empty when no packet expected
	local outer_dst="2001:db8:f::1"
	local out

	# fresh prerouting chain so each invocation starts clean
	ip netns exec "$srgw" nft flush chain ip filter prerouting
	ip netns exec "$srgw" nft add rule ip filter prerouting \
		ip saddr 10.0.0.1 "$verdict"

	out=$(mktemp)
	capture_traffic "$srupf" "veth-n9-srupf" "$outer_dst" "$out"

	if [ -n "$expect_srh0" ]; then
		EXPECTED_SRH0="$expect_srh0" python3 - "$out" <<'PYEOF'
import ipaddress, os, sys
from scapy.all import rdpcap, IPv6, IPv6ExtHdrSegmentRouting

expected_srh0 = os.environ['EXPECTED_SRH0']
pkts = rdpcap(sys.argv[1])
for p in pkts:
    if not (IPv6 in p and IPv6ExtHdrSegmentRouting in p):
        continue
    srh = p[IPv6ExtHdrSegmentRouting]
    if len(srh.addresses) < 2:
        continue
    srh0 = ipaddress.IPv6Address(str(srh.addresses[0])).packed
    if srh0 == ipaddress.IPv6Address(expected_srh0).packed:
        sys.exit(0)
sys.exit("expected SRv6 packet not observed at upf despite nft accept")
PYEOF
	else
		python3 - "$out" <<'PYEOF'
import sys
from scapy.all import rdpcap, IPv6, IPv6ExtHdrSegmentRouting

pkts = rdpcap(sys.argv[1])
for p in pkts:
    if IPv6 in p and IPv6ExtHdrSegmentRouting in p:
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

	if run_test "2001:db8:f::1" "2001:db8:f::1"; then
		echo "TEST: End.M.GTP6.D (default) [PASS]"
	else
		echo "TEST: End.M.GTP6.D (default) [FAIL]"
		rc=1
	fi

	if run_echo_test "2001:db8:f::1"; then
		echo "TEST: End.M.GTP6.D (non-T-PDU passthrough) [PASS]"
	else
		echo "TEST: End.M.GTP6.D (non-T-PDU passthrough) [FAIL]"
		rc=1
	fi

	# VRF binding: SRv6 underlay output goes through vrf-n9 (table 100).
	# Reported as [SKIP] when CONFIG_NET_VRF is not loaded.
	if [ "$have_vrf" = "1" ]; then
		if run_test "2001:db8:f0::1" "2001:db8:f0::1" \
			    "$srupf_vrf" "veth-n9-2-srupf"; then
			echo "TEST: End.M.GTP6.D (oif vrf-n9) [PASS]"
		else
			echo "TEST: End.M.GTP6.D (oif vrf-n9) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP6.D (oif vrf-n9) [SKIP] (CONFIG_NET_VRF not loaded)"
	fi

	# Inner T-PDU netfilter hook: only meaningful when nft is present
	# and the kernel exposes net.netfilter.nf_hooks_lwtunnel.  The
	# sysctl is one-way (cannot be cleared), but each test runs in a
	# fresh netns so this is harmless.
	if command -v nft >/dev/null && \
	   ip netns exec "$srgw" sysctl -wq \
		net.netfilter.nf_hooks_lwtunnel=1 2>/dev/null; then
		ip netns exec "$srgw" nft add table ip filter
		ip netns exec "$srgw" nft \
			'add chain ip filter prerouting { type filter hook prerouting priority 0; }'

		if run_nf_test drop ""; then
			echo "TEST: End.M.GTP6.D (nft drop on inner) [PASS]"
		else
			echo "TEST: End.M.GTP6.D (nft drop on inner) [FAIL]"
			rc=1
		fi

		if run_nf_test accept "2001:db8:f::1"; then
			echo "TEST: End.M.GTP6.D (nft accept on inner) [PASS]"
		else
			echo "TEST: End.M.GTP6.D (nft accept on inner) [FAIL]"
			rc=1
		fi
	else
		echo "TEST: End.M.GTP6.D (inner-flow netfilter hook) [SKIP]" \
		     "(nft or nf_hooks_lwtunnel unavailable)"
	fi

	if [ "$rc" -eq 0 ]; then
		echo "TEST: End.M.GTP6.D [PASS]"
		exit "$ksft_pass"
	else
		echo "TEST: End.M.GTP6.D [FAIL]"
		exit "$ksft_fail"
	fi
}

main "$@"
