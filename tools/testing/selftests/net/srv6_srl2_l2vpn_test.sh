#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# author: Andrea Mayer <andrea.mayer@uniroma2.it>
#
# This script tests the full SRv6 L2 VPN data path using the srl2
# virtual Ethernet device for L2 frame encapsulation and the End.DT2U
# behavior (RFC 8986, Section 4.11) for decapsulation.
#
# This test exercises the full SRv6 L2 VPN data path using the srl2
# device as the TX-side encapsulator. Each SRv6 router uses a bridge
# (br0) with two ports:
#   - veth-hs: connects to the host
#   - srl2-0: SRv6 L2 tunnel device for encap/decap
#
# TX path: the host sends an L2 frame via veth-hs. The bridge forwards
# the frame to srl2-0 (L2 forwarding based on dst MAC). srl2-0
# encapsulates the frame in IPv6+SRH and transmits.
#
# RX path: an SRv6 packet arrives carrying an inner Ethernet frame.
# End.DT2U decapsulates and delivers the frame on srl2-0 via
# netif_rx(). Since srl2-0 is a bridge port, the bridge performs MAC
# learning and L2 forwarding to deliver the frame to veth-hs.
#
# Note: no static MAC addresses or neighbor entries are needed here.
# ARP works naturally through the
# bridge and the SRv6 tunnel: ARP requests are broadcast, flooded by
# the bridge to srl2-0, encapsulated, decapsulated by End.DT2U on the
# remote side, and flooded to the destination host.
#
# Topology:
#
#              cafe::1                                cafe::2
#             10.0.0.1                               10.0.0.2
#            +--------+                             +--------+
#            |        |                             |        |
#            |  hs-1  |                             |  hs-2  |
#            |        |                             |        |
#            +---+----+                             +----+---+
#   cafe::/64    |                                       |      cafe::/64
# 10.0.0.0/24    |                                       |    10.0.0.0/24
#          +-----+------+                         +------+-----+
#          |   veth-hs  |                         |  veth-hs   |
#          |      |     |   fcf0:0:1:2::/64       |     |      |
#          |    br0     +-------------------------+   br0      |
#          |      |     |                         |     |      |
#          |   srl2-0   |                         |  srl2-0    |
#          |    rt-1    |                         |   rt-2     |
#          +------------+                         +------------+
#
#
# Every fcf0:0:x:y::/64 network interconnects the SRv6 routers rt-x with
# rt-y in the IPv6 operator network.
#
# Local SID table
# ===============
#
# Each SRv6 router is configured with a Local SID table in which SIDs are
# stored. Considering the given SRv6 router rt-x, the following SID is
# configured in the Local SID table:
#
#   Local SID table for SRv6 router rt-x
#   +-----------------------------------------------------------+
#   |fcff:x::d20 is associated with the SRv6 End.DT2U behavior |
#   +-----------------------------------------------------------+
#
# SRv6 L2 encapsulation
# =====================
#
# Each router's srl2-0 device is configured with a SID list pointing to
# the remote router's End.DT2U SID:
#
#   rt-1 srl2-0: segs fcff:2::d20  (encap towards rt-2)
#   rt-2 srl2-0: segs fcff:1::d20  (encap towards rt-1)
#
# Each SID list consists of only one SID. The srl2 device encapsulates
# the L2 frame in an outer IPv6 header with an SRH containing the
# segment list.
#

source lib.sh

readonly DUMMY_DEVNAME="dum0"
readonly SRL2_DEVNAME="srl2-0"
readonly RT2HS_DEVNAME="veth-hs"
readonly BRIDGE_DEVNAME="br0"
readonly HS_VETH_NAME="veth0"
readonly LOCALSID_TABLE_ID=90
readonly IPv6_RT_NETWORK=fcf0:0
readonly IPv6_HS_NETWORK=cafe
readonly IPv4_HS_NETWORK=10.0.0
readonly VPN_LOCATOR_SERVICE=fcff
readonly DT2U_FUNC=0d20

PING_TIMEOUT_SEC=4
PAUSE_ON_FAIL=${PAUSE_ON_FAIL:=no}

ROUTERS=''
HOSTS=''

SETUP_ERR=1

ret=${ksft_skip}
nsuccess=0
nfail=0

log_test()
{
	local rc="$1"
	local expected="$2"
	local msg="$3"

	if [ "${rc}" -eq "${expected}" ]; then
		nsuccess=$((nsuccess+1))
		printf "\n    TEST: %-60s  [ OK ]\n" "${msg}"
	else
		ret=1
		nfail=$((nfail+1))
		printf "\n    TEST: %-60s  [FAIL]\n" "${msg}"
		if [ "${PAUSE_ON_FAIL}" = "yes" ]; then
			echo
			echo "hit enter to continue, 'q' to quit"
			read a
			[ "$a" = "q" ] && exit 1
		fi
	fi
}

print_log_test_results()
{
	printf "\nTests passed: %3d\n" "${nsuccess}"
	printf "Tests failed: %3d\n"   "${nfail}"

	if [ "${ret}" -ne 1 ]; then
		ret=0
	fi
}

log_section()
{
	echo
	echo "################################################################################"
	echo "TEST SECTION: $*"
	echo "################################################################################"
}

test_command_or_ksft_skip()
{
	local cmd="$1"

	if [ ! -x "$(command -v "${cmd}")" ]; then
		echo "SKIP: Could not run test without \"${cmd}\" tool";
		exit "${ksft_skip}"
	fi
}

get_rtname()
{
	local rtid="$1"

	echo "rt_${rtid}"
}

get_hsname()
{
	local hsid="$1"

	echo "hs_${hsid}"
}

create_router()
{
	local rtid="$1"
	local nsname

	nsname="$(get_rtname "${rtid}")"
	setup_ns "${nsname}"
}

create_host()
{
	local hsid="$1"
	local nsname

	nsname="$(get_hsname "${hsid}")"
	setup_ns "${nsname}"
}

cleanup()
{
	cleanup_all_ns

	if [ "${SETUP_ERR}" -ne 0 ]; then
		echo "SKIP: Setting up the testing environment failed"
		exit "${ksft_skip}"
	fi

	exit "${ret}"
}

add_link_rt_pairs()
{
	local rt="$1"
	local rt_neighs="$2"
	local neigh
	local nsname
	local neigh_nsname

	eval nsname=\${$(get_rtname "${rt}")}

	for neigh in ${rt_neighs}; do
		eval neigh_nsname=\${$(get_rtname "${neigh}")}

		ip link add "veth-rt-${rt}-${neigh}" netns "${nsname}" \
			type veth peer name "veth-rt-${neigh}-${rt}" \
			netns "${neigh_nsname}"
	done
}

get_network_prefix()
{
	local rt="$1"
	local neigh="$2"
	local p="${rt}"
	local q="${neigh}"

	if [ "${p}" -gt "${q}" ]; then
		p="${q}"; q="${rt}"
	fi

	echo "${IPv6_RT_NETWORK}:${p}:${q}"
}

setup_rt_networking()
{
	local rt="$1"
	local rt_neighs="$2"
	local nsname
	local net_prefix
	local devname
	local neigh

	eval nsname=\${$(get_rtname "${rt}")}

	for neigh in ${rt_neighs}; do
		devname="veth-rt-${rt}-${neigh}"

		net_prefix="$(get_network_prefix "${rt}" "${neigh}")"

		ip -netns "${nsname}" addr \
			add "${net_prefix}::${rt}/64" dev "${devname}" nodad

		ip -netns "${nsname}" link set "${devname}" up
	done

	ip -netns "${nsname}" link add "${DUMMY_DEVNAME}" type dummy

	ip -netns "${nsname}" link set "${DUMMY_DEVNAME}" up
	ip -netns "${nsname}" link set lo up

	ip netns exec "${nsname}" sysctl -wq net.ipv6.conf.all.accept_dad=0
	ip netns exec "${nsname}" sysctl -wq net.ipv6.conf.default.accept_dad=0
	ip netns exec "${nsname}" sysctl -wq net.ipv6.conf.all.forwarding=1
	ip netns exec "${nsname}" sysctl -wq net.ipv4.ip_forward=1
}

setup_rt_local_sids()
{
	local rt="$1"
	local rt_neighs="$2"
	local net_prefix
	local devname
	local nsname
	local neigh

	eval nsname=\${$(get_rtname "${rt}")}

	for neigh in ${rt_neighs}; do
		devname="veth-rt-${rt}-${neigh}"

		net_prefix="$(get_network_prefix "${rt}" "${neigh}")"

		# set underlay network routes for SIDs reachability
		ip -netns "${nsname}" -6 route \
			add "${VPN_LOCATOR_SERVICE}:${neigh}::/32" \
			table "${LOCALSID_TABLE_ID}" \
			via "${net_prefix}::${neigh}" dev "${devname}"
	done

	# Local End.DT2U behavior: decapsulate L2 frames and deliver on
	# srl2-0 which is a bridge port; the bridge then forwards to the
	# host connected via veth-hs.
	ip -netns "${nsname}" -6 route \
		add "${VPN_LOCATOR_SERVICE}:${rt}::${DT2U_FUNC}" \
		table "${LOCALSID_TABLE_ID}" \
		encap seg6local action End.DT2U l2dev "${SRL2_DEVNAME}" \
		dev "${DUMMY_DEVNAME}"

	# all SIDs for VPNs start with a common locator. Routes and SRv6
	# Endpoint behaviors instances are grouped together in the 'localsid'
	# table.
	ip -netns "${nsname}" -6 rule add \
		to "${VPN_LOCATOR_SERVICE}::/16" \
		lookup "${LOCALSID_TABLE_ID}" prio 999
}

setup_hs()
{
	local hs="$1"
	local rt="$2"
	local hsname
	local rtname

	eval hsname=\${$(get_hsname "${hs}")}
	eval rtname=\${$(get_rtname "${rt}")}

	ip netns exec "${hsname}" sysctl -wq net.ipv6.conf.all.accept_dad=0
	ip netns exec "${hsname}" sysctl -wq net.ipv6.conf.default.accept_dad=0

	ip -netns "${hsname}" link add "${HS_VETH_NAME}" type veth \
		peer name "${RT2HS_DEVNAME}" netns "${rtname}"

	ip -netns "${hsname}" addr add "${IPv6_HS_NETWORK}::${hs}/64" \
		dev "${HS_VETH_NAME}" nodad
	ip -netns "${hsname}" addr add "${IPv4_HS_NETWORK}.${hs}/24" \
		dev "${HS_VETH_NAME}"

	ip -netns "${hsname}" link set "${HS_VETH_NAME}" up
	ip -netns "${hsname}" link set lo up

	# veth-hs is a bridge port; IPs go on br0 (see setup_bridge)
	ip -netns "${rtname}" link set "${RT2HS_DEVNAME}" up
}

# Create srl2 device, bridge, and wire them together.
# The srl2 device encapsulates L2 frames in IPv6+SRH towards the
# remote router's End.DT2U SID. The bridge connects the host (via
# veth-hs) with the SRv6 tunnel (via srl2-0).
# args:
#  $1 - router id
#  $2 - remote router id (for SID list)
setup_bridge()
{
	local rt="$1"
	local remote_rt="$2"
	local nsname

	eval nsname=\${$(get_rtname "${rt}")}

	# create the srl2 device pointing to the remote End.DT2U SID
	ip -netns "${nsname}" link add "${SRL2_DEVNAME}" type srl2 \
		segs "${VPN_LOCATOR_SERVICE}:${remote_rt}::${DT2U_FUNC}"
	ip -netns "${nsname}" link set "${SRL2_DEVNAME}" up

	# create bridge and add ports
	ip -netns "${nsname}" link add "${BRIDGE_DEVNAME}" type bridge
	ip -netns "${nsname}" link set "${BRIDGE_DEVNAME}" up

	ip -netns "${nsname}" link set "${RT2HS_DEVNAME}" master \
		"${BRIDGE_DEVNAME}"
	ip -netns "${nsname}" link set "${SRL2_DEVNAME}" master \
		"${BRIDGE_DEVNAME}"

	# IP addresses on br0 (gateway for the hosts)
	ip -netns "${nsname}" addr add "${IPv6_HS_NETWORK}::254/64" \
		dev "${BRIDGE_DEVNAME}" nodad
	ip -netns "${nsname}" addr \
		add "${IPv4_HS_NETWORK}.254/24" dev "${BRIDGE_DEVNAME}"
}

setup()
{
	local i

	# create routers
	ROUTERS="1 2"; readonly ROUTERS
	for i in ${ROUTERS}; do
		create_router "${i}"
	done

	# create hosts
	HOSTS="1 2"; readonly HOSTS
	for i in ${HOSTS}; do
		create_host "${i}"
	done

	# set up the links for connecting routers
	add_link_rt_pairs 1 "2"

	# set up the basic connectivity of routers and routes required for
	# reachability of SIDs.
	setup_rt_networking 1 "2"
	setup_rt_networking 2 "1"

	# set up the hosts connected to routers
	setup_hs 1 1
	setup_hs 2 2

	# set up srl2 devices and bridges on each router.
	# rt-1's srl2-0 encapsulates towards rt-2's End.DT2U SID and
	# vice versa.
	setup_bridge 1 2
	setup_bridge 2 1

	# set up SRv6 Endpoints (i.e. SRv6 End.DT2U)
	setup_rt_local_sids 1 "2"
	setup_rt_local_sids 2 "1"

	# testing environment was set up successfully
	SETUP_ERR=0
}

check_rt_connectivity()
{
	local rtsrc="$1"
	local rtdst="$2"
	local prefix
	local rtsrc_nsname

	eval rtsrc_nsname=\${$(get_rtname "${rtsrc}")}

	prefix="$(get_network_prefix "${rtsrc}" "${rtdst}")"

	ip netns exec "${rtsrc_nsname}" ping -c 1 -W "${PING_TIMEOUT_SEC}" \
		"${prefix}::${rtdst}" >/dev/null 2>&1
}

check_and_log_rt_connectivity()
{
	local rtsrc="$1"
	local rtdst="$2"

	check_rt_connectivity "${rtsrc}" "${rtdst}"
	log_test $? 0 "Routers connectivity: rt-${rtsrc} -> rt-${rtdst}"
}

check_hs_ipv6_connectivity()
{
	local hssrc="$1"
	local hsdst="$2"
	local hssrc_nsname

	eval hssrc_nsname=\${$(get_hsname "${hssrc}")}

	ip netns exec "${hssrc_nsname}" ping -c 1 -W "${PING_TIMEOUT_SEC}" \
		"${IPv6_HS_NETWORK}::${hsdst}" >/dev/null 2>&1
}

check_hs_ipv4_connectivity()
{
	local hssrc="$1"
	local hsdst="$2"
	local hssrc_nsname

	eval hssrc_nsname=\${$(get_hsname "${hssrc}")}

	ip netns exec "${hssrc_nsname}" ping -c 1 -W "${PING_TIMEOUT_SEC}" \
		"${IPv4_HS_NETWORK}.${hsdst}" >/dev/null 2>&1
}

check_and_log_hs2gw_connectivity()
{
	local hssrc="$1"

	check_hs_ipv6_connectivity "${hssrc}" 254
	log_test $? 0 "IPv6 Hosts connectivity: hs-${hssrc} -> gw"

	check_hs_ipv4_connectivity "${hssrc}" 254
	log_test $? 0 "IPv4 Hosts connectivity: hs-${hssrc} -> gw"
}

check_and_log_hs_ipv6_connectivity()
{
	local hssrc="$1"
	local hsdst="$2"

	check_hs_ipv6_connectivity "${hssrc}" "${hsdst}"
	log_test $? 0 "IPv6 Hosts connectivity: hs-${hssrc} -> hs-${hsdst}"
}

check_and_log_hs_ipv4_connectivity()
{
	local hssrc="$1"
	local hsdst="$2"

	check_hs_ipv4_connectivity "${hssrc}" "${hsdst}"
	log_test $? 0 "IPv4 Hosts connectivity: hs-${hssrc} -> hs-${hsdst}"
}

check_and_log_hs_connectivity()
{
	local hssrc="$1"
	local hsdst="$2"

	check_and_log_hs_ipv4_connectivity "${hssrc}" "${hsdst}"
	check_and_log_hs_ipv6_connectivity "${hssrc}" "${hsdst}"
}

router_tests()
{
	local i
	local j

	log_section "IPv6 routers connectivity test"

	for i in ${ROUTERS}; do
		for j in ${ROUTERS}; do
			if [ "${i}" -eq "${j}" ]; then
				continue
			fi

			check_and_log_rt_connectivity "${i}" "${j}"
		done
	done
}

host2gateway_tests()
{
	local hs

	log_section "IPv4/IPv6 connectivity test among hosts and gateways"

	for hs in ${HOSTS}; do
		check_and_log_hs2gw_connectivity "${hs}"
	done
}

host_vpn_tests()
{
	log_section "SRv6 srl2 + End.DT2U L2 VPN connectivity test hosts (h1 <-> h2)"

	check_and_log_hs_connectivity 1 2
	check_and_log_hs_connectivity 2 1
}

test_dummy_dev_or_ksft_skip()
{
	local test_netns

	test_netns="dummy-$(mktemp -u XXXXXXXX)"

	if ! ip netns add "${test_netns}"; then
		echo "SKIP: Cannot set up netns for testing dummy dev support"
		exit "${ksft_skip}"
	fi

	modprobe dummy &>/dev/null || true
	if ! ip -netns "${test_netns}" link \
		add "${DUMMY_DEVNAME}" type dummy; then
		echo "SKIP: dummy dev not supported"

		ip netns del "${test_netns}"
		exit "${ksft_skip}"
	fi

	ip netns del "${test_netns}"
}

test_srl2_dev_or_ksft_skip()
{
	local test_netns

	test_netns="srl2-$(mktemp -u XXXXXXXX)"

	if ! ip netns add "${test_netns}"; then
		echo "SKIP: Cannot set up netns for testing srl2 dev support"
		exit "${ksft_skip}"
	fi

	modprobe srl2 &>/dev/null || true
	if ! ip -netns "${test_netns}" link \
		add srl2-test type srl2 \
		segs fc00::1; then
		echo "SKIP: srl2 dev not supported"

		ip netns del "${test_netns}"
		exit "${ksft_skip}"
	fi

	ip netns del "${test_netns}"
}

test_iproute2_supp_or_ksft_skip()
{
	if ! ip route help 2>&1 | grep -qo "End.DT2U"; then
		echo "SKIP: Missing SRv6 End.DT2U support in iproute2"
		exit "${ksft_skip}"
	fi

	if ! ip link help srl2 2>&1 | grep -qo "srl2"; then
		echo "SKIP: Missing srl2 link type support in iproute2"
		exit "${ksft_skip}"
	fi
}

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: Need root privileges"
	exit "${ksft_skip}"
fi

# required programs to carry out this selftest
test_command_or_ksft_skip ip
test_command_or_ksft_skip ping
test_command_or_ksft_skip sysctl
test_command_or_ksft_skip grep

test_iproute2_supp_or_ksft_skip
test_dummy_dev_or_ksft_skip
test_srl2_dev_or_ksft_skip

set -e
trap cleanup EXIT

setup
set +e

router_tests
host2gateway_tests
host_vpn_tests

print_log_test_results
