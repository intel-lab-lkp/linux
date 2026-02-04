#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tests that ARP announcements with Broadcast or NULL mac are never
# accepted
#

source lib.sh

readonly V4_ADDR0="10.0.10.1"
readonly V6_ADDR0="fd00:1::1"
readonly V4_ADDR1="10.0.10.2"
readonly V6_ADDR1="fd00:1::2"
readonly V6_ALL_NODES="ff02::1"
readonly V6_SOL_NODE1="ff02::1:ff00:0002"
readonly BCAST_MAC="ff:ff:ff:ff:ff:ff"
readonly MCAST_MAC="01:00:5e:00:00:00"
readonly NULL_MAC="00:00:00:00:00:00"
readonly VALID_MAC="02:01:02:03:04:05"
readonly V6_ALL_NODE_MAC="33:33:FF:00:00:01"
readonly V6_SOL_NODE_MAC1="33:33:FF:00:00:02"
readonly NS=135
readonly NA=136
readonly ARP_REQ=request
readonly ARP_REPLY=reply
ret=0
veth0_ifindex=0
veth1_mac=

setup() {
	setup_ns PEER_NS

	ip link add name veth0 type veth peer name veth1
	ip link set dev veth0 up
	ip link set dev veth1 netns "${PEER_NS}"
	ip netns exec "${PEER_NS}" ip link set dev veth1 up
	ip addr add "${V4_ADDR0}"/24 dev veth0
	ip addr add "${V6_ADDR0}"/64 dev veth0
	ip netns exec "${PEER_NS}" ip addr add "${V4_ADDR1}"/24 dev veth1
	ip netns exec "${PEER_NS}" ip route add default via "${V4_ADDR0}" dev veth1

	ip netns exec "${PEER_NS}" ip addr add "${V6_ADDR1}"/64 dev veth1
	ip netns exec "${PEER_NS}" ip route add default via "${V6_ADDR0}" dev veth1

	# Raise ARP timers to avoid flakes due to refreshes
	sysctl -w net.ipv4.neigh.veth0.base_reachable_time=3600 \
		>/dev/null 2>&1
	ip netns exec "${PEER_NS}" \
		sysctl -w net.ipv4.neigh.veth1.gc_stale_time=3600 \
		>/dev/null 2>&1
	ip netns exec "${PEER_NS}" \
		sysctl -w net.ipv4.neigh.veth1.base_reachable_time=3600 \
		>/dev/null 2>&1

	veth0_ifindex=$(ip -j link show veth0 | jq -r '.[0].ifindex')
	veth1_mac="$(ip netns exec "${PEER_NS}" ip -j link show veth1 | \
		jq -r '.[0].address' )"
}

cleanup() {
	ip neigh flush dev veth0
	ip link del veth0
	cleanup_ns "${PEER_NS}"
}

# Make sure ARP announcement with invalid MAC is never learnt
run_no_arp_poisoning() {
	local l2_dmac="${1}"
	local tmac="${2}"
	local op="${3}"

	ret=0

	ip netns exec "${PEER_NS}" ip neigh flush dev veth1 >/dev/null 2>&1
	ip netns exec "${PEER_NS}" ping -c 1 "${V4_ADDR0}" >/dev/null 2>&1

	# Poison with a valid MAC to ensure injection is working
	mausezahn "veth0" -q -a "${VALID_MAC}" -b "${BCAST_MAC}" -t arp \
		  "${op}, sip=${V4_ADDR0}, tip=${V4_ADDR0}, smac=${VALID_MAC}, tmac=${VALID_MAC}"

	neigh=$(ip netns exec "${PEER_NS}" ip neigh show "${V4_ADDR0}" | \
		grep "${VALID_MAC}")
	if [ "${neigh}" == "" ]; then
		echo "ERROR: unable to ARP poision with a valid MAC ${VALID_MAC}"
		ip netns exec "${PEER_NS}" ip neigh show "${V4_ADDR0}"
		ret=1
		return
	fi

	# Poison with tmac
	mausezahn "veth0" -q -a "${VALID_MAC}" -b "${l2_dmac}" -t arp \
		  "${op}, sip=${V4_ADDR0}, tip=${V4_ADDR0}, smac=${tmac}, tmac=${tmac}"


	neigh=$(ip netns exec "${PEER_NS}" ip neigh show "${V4_ADDR0}" | \
		grep "${tmac}")
	if [ "${neigh}" != "" ]; then
		echo "ERROR: ARP entry learnt for ${tmac} announcement."
		ip netns exec "${PEER_NS}" ip neigh show "${V4_ADDR0}"
		ret=1
		return
	fi
}

# Make sure NDP announcement with invalid MAC is never learnt
run_no_ndp_poisoning() {
	local l2_dmac="${1}"
	local dst_ip="${2}"
	local op="${3}"
	local tip="${V6_ADDR0}"
	local tmac="${4}"

	if [ "${op}" == "${NS}" ]; then
		tip="${V6_ADDR1}"
	fi

	ret=0

	ip netns exec "${PEER_NS}" ip -6 neigh flush dev veth1 >/dev/null 2>&1
	ip netns exec "${PEER_NS}" ping -c 1 "${V6_ADDR0}" >/dev/null 2>&1

	# Poison with a valid MAC to ensure injection is working
	./ndisc_send "${veth0_ifindex}" "${l2_dmac}" "${VALID_MAC}" "${dst_ip}" \
		"${V6_ADDR0}" "${tip}" "${op}" "${VALID_MAC}"
	neigh=$(ip netns exec "${PEER_NS}" ip neigh show "${V6_ADDR0}" | \
		grep "${VALID_MAC}")
	if [ "${neigh}" == "" ]; then
		echo "ERROR: unable to NDP poision with a valid MAC ${VALID_MAC}"
		ip netns exec "${PEER_NS}" ip neigh show "${V6_ADDR0}"
		ret=1
		return
	fi

	# Poison with tmac
	./ndisc_send "${veth0_ifindex}" "${l2_dmac}" "${VALID_MAC}" "${dst_ip}" \
		"${V6_ADDR0}" "${tip}" "${op}" "${tmac}"
	neigh=$(ip netns exec "${PEER_NS}" ip neigh show "${V6_ADDR0}" | \
		grep "${tmac}")
	if [ "${neigh}" != "" ]; then
		echo "ERROR: NDP entry learnt for ${tmac} announcement."
		ip netns exec "${PEER_NS}" ip neigh show "${V6_ADDR0}"
		ret=1
		return
	fi
}

print_test_result() {
	local msg="${1}"
	local rc="${2}"

	if [ "${rc}" == 0 ]; then
		printf "TEST: %-60s  [ OK ]" "${msg}"
	else
		printf "TEST: %-60s  [ FAIL ]" "${msg}"
	fi
}

run_all_tests() {
	local results

	setup

	## ARP
	# Broadcast gARPs
	msg="1.1  ARP no poisoning dmac=bcast reply sha=bcast"
	run_no_arp_poisoning "${BCAST_MAC}" "${BCAST_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.2  ARP no poisoning dmac=bcast reply sha=null"
	run_no_arp_poisoning "${BCAST_MAC}" "${NULL_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.3  ARP no poisoning dmac=bcast req   sha=bcast"
	run_no_arp_poisoning "${BCAST_MAC}" "${BCAST_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.4  ARP no poisoning dmac=bcast req   sha=null"
	run_no_arp_poisoning "${BCAST_MAC}" "${NULL_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.5  ARP no poisoning dmac=bcast req   sha=mcast"
	run_no_arp_poisoning "${BCAST_MAC}" "${MCAST_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.6  ARP no poisoning dmac=bcast reply sha=mcast"
	run_no_arp_poisoning "${BCAST_MAC}" "${MCAST_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Targeted gARPs
	msg="1.7  ARP no poisoning dmac=veth0 reply sha=bcast"
	run_no_arp_poisoning "${veth1_mac}" "${BCAST_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.8  ARP no poisoning dmac=veth0 reply sha=null"
	run_no_arp_poisoning "${veth1_mac}" "${NULL_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.9  ARP no poisoning dmac=veth0 req   sha=bcast"
	run_no_arp_poisoning "${veth1_mac}" "${BCAST_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.10 ARP no poisoning dmac=veth0 req   sha=null"
	run_no_arp_poisoning "${veth1_mac}" "${NULL_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.11 ARP no poisoning dmac=veth0 req   sha=mcast"
	run_no_arp_poisoning "${veth1_mac}" "${MCAST_MAC}" "${ARP_REQ}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="1.12 ARP no poisoning dmac=veth0 reply sha=mcast"
	run_no_arp_poisoning "${veth1_mac}" "${MCAST_MAC}" "${ARP_REPLY}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	### NDP
	## NA
	# Broadcast / All node MAC, all-node IP announcements
	msg="2.1  NDP no poisoning dmac=bcast   all_nodes na lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ALL_NODES}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.2  NDP no poisoning dmac=bcast   all_nodes na lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ALL_NODES}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.3  NDP no poisoning dmac=allnode all_nodes na lladdr=bcast"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ALL_NODES}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.4  NDP no poisoning dmac=allnode all_nodes na lladdr=null"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ALL_NODES}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.5  NDP no poisoning dmac=bcast   all_nodes na lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ALL_NODES}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.6  NDP no poisoning dmac=bcast   all_nodes na lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ALL_NODES}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.7  NDP no poisoning dmac=allnode all_nodes na lladdr=bcast"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ALL_NODES}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.8  NDP no poisoning dmac=allnode all_nodes na lladdr=null"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ALL_NODES}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Broadcast / All node MAC, Targeted IP announce
	msg="2.9  NDP no poisoning dmac=bcast   targeted  na lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.10 NDP no poisoning dmac=bcast   targeted  na lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.11 NDP no poisoning dmac=allnode targeted  na lladdr=bcast"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ADDR1}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.12 NDP no poisoning dmac=allnode targeted  na lladdr=null"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ADDR1}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.13 NDP no poisoning dmac=bcast   targeted  na lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.14 NDP no poisoning dmac=bcast   targeted  na lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.15 NDP no poisoning dmac=allnode targeted  na lladdr=bcast"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ADDR1}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.16 NDP no poisoning dmac=allnode targeted  na lladdr=null"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ADDR1}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Targeted MAC, Targeted IP announce
	msg="2.17 NDP no poisoning dmac=veth1   targeted  na lladdr=bcast"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NA}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.18 NDP no poisoning dmac=veth1   targeted  na lladdr=null"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NA}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Poison with MCAST mac
	msg="2.19 NDP no poisoning dmac=allnode all_nodes na lladdr=mcast"
	run_no_ndp_poisoning "${V6_ALL_NODE_MAC}" "${V6_ALL_NODES}" "${NA}" "${MCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"
	msg="2.20 NDP no poisoning dmac=veth1   targeted  na lladdr=mcast"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NA}" "${MCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	## NS
	# Broadcast / SolNode node MAC, SolNode IP solic
	msg="2.21 NDP no poisoning dmac=bcast   solnode   ns lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_SOL_NODE1}" "${NS}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.22 NDP no poisoning dmac=bcast   solnode   ns lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_SOL_NODE1}" "${NS}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.23 NDP no poisoning dmac=solnode solnode   ns lladdr=bcast"
	run_no_ndp_poisoning "${V6_SOL_NODE_MAC1}" "${V6_SOL_NODE1}" "${NS}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.24 NDP no poisoning dmac=solnode solnode   ns lladdr=null"
	run_no_ndp_poisoning "${V6_SOL_NODE_MAC1}" "${V6_SOL_NODE1}" "${NS}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Broadcast / SolNode node MAC, target IP solic
	msg="2.25 NDP no poisoning dmac=bcast   target    ns lladdr=bcast"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NS}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.26 NDP no poisoning dmac=bcast   target    ns lladdr=null"
	run_no_ndp_poisoning "${BCAST_MAC}" "${V6_ADDR1}" "${NS}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.27 NDP no poisoning dmac=solnode target    ns lladdr=bcast"
	run_no_ndp_poisoning "${V6_SOL_NODE_MAC1}" "${V6_ADDR1}" "${NS}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.28 NDP no poisoning dmac=solnode target    ns lladdr=null"
	run_no_ndp_poisoning "${V6_SOL_NODE_MAC1}" "${V6_ADDR1}" "${NS}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Targeted MAC, Targeted IP solic
	msg="2.29 NDP no poisoning dmac=veth1   target    ns lladdr=bcast"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NS}" "${BCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.30 NDP no poisoning dmac=veth1   target    ns lladdr=null"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NS}" "${NULL_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	# Poison with MCAST mac
	msg="2.31 NDP no poisoning dmac=solnode solnode   ns lladdr=mcast"
	run_no_ndp_poisoning "${V6_SOL_NODE_MAC1}" "${V6_SOL_NODE1}" "${NS}" "${MCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	msg="2.32 NDP no poisoning dmac=veth1   target    ns lladdr=mcast"
	run_no_ndp_poisoning "${veth1_mac}" "${V6_ADDR1}" "${NS}" "${MCAST_MAC}"
	results+="$(print_test_result "${msg}" "${ret}")\n"

	cleanup

	printf '%b' "${results}"
}

if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit "${ksft_skip}"
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool"
	exit "${ksft_skip}"
fi

run_all_tests
exit "${ret}"
