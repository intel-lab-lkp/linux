#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tests that ARP announcements with Broadcast, Multicast or NULL mac are never
# accepted
#

source lib.sh

readonly V4_ADDR0="10.0.10.1"
readonly V4_ADDR1="10.0.10.2"
readonly BCAST_MAC="ff:ff:ff:ff:ff:ff"
readonly MCAST_MAC="01:00:5e:00:00:00"
readonly NULL_MAC="00:00:00:00:00:00"
readonly VALID_MAC="02:01:02:03:04:05"
readonly ARP_REQ="request"
readonly ARP_REPLY="reply"
ret=0
veth1_mac=

setup() {
	setup_ns PEER_NS

	ip link add name veth0 type veth peer name veth1
	ip link set dev veth0 up
	ip link set dev veth1 netns "${PEER_NS}"
	ip netns exec "${PEER_NS}" ip link set dev veth1 up
	ip addr add "${V4_ADDR0}"/24 dev veth0
	ip netns exec "${PEER_NS}" ip addr add "${V4_ADDR1}"/24 dev veth1
	ip netns exec "${PEER_NS}" ip route add default via "${V4_ADDR0}" dev veth1

	# Raise ARP timers to avoid flakes due to refreshes
	sysctl -w net.ipv4.neigh.veth0.base_reachable_time=3600 \
		>/dev/null 2>&1
	ip netns exec "${PEER_NS}" \
		sysctl -w net.ipv4.neigh.veth1.gc_stale_time=3600 \
		>/dev/null 2>&1
	ip netns exec "${PEER_NS}" \
		sysctl -w net.ipv4.neigh.veth1.base_reachable_time=3600 \
		>/dev/null 2>&1

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
