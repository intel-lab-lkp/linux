#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2020-2025 OpenVPN, Inc.
#
#	Author:	Ralf Lici <ralf@mandelbit.com>
#		Antonio Quartulli <antonio@openvpn.net>

#set -x
set -e

PROTO=UDP
source ./common.sh

BIND_TYPE=${BIND_TYPE:-"DEV"}

cleanup

modprobe -q ovpn || true

# setup a P2P session between peer1 and peer2

ip netns add peer1
ip netns add peer2

ip link add veth1 netns peer1 type veth peer name veth1 netns peer2
ip link add veth2 netns peer1 type veth peer name veth2 netns peer2

ip -n peer1 addr add 10.10.10.1/24 dev veth1
ip -n peer1 link set veth1 up

ip -n peer1 addr add 20.20.20.1/24 dev veth2
ip -n peer1 link set veth2 up

ip -n peer2 addr add 10.10.10.2/24 dev veth1
ip -n peer2 link set veth1 up

ip -n peer2 addr add 20.20.20.2/24 dev veth2
ip -n peer2 link set veth2 up

ip netns exec peer1 ${OVPN_CLI} new_iface tun1 P2P
ip netns exec peer2 ${OVPN_CLI} new_iface tun2 P2P

ip -n peer1 addr add 5.5.5.1 dev tun1
ip -n peer1 link set tun1 up
ip -n peer2 addr add 5.5.5.2 dev tun2
ip -n peer2 link set tun2 up

ip -n peer1 route add 5.5.5.0/24 dev tun1
ip -n peer2 route add 5.5.5.0/24 dev tun2

run_bind_test() {
	dev1=${1}
	dev2=${2}
	raddr4_peer1=${3}
	raddr4_peer2=${4}

	touch /tmp/ovpn-bind1.log
	touch /tmp/ovpn-bind2.log

	ip netns exec peer1 ${OVPN_CLI} del_peer tun1 1 2>/dev/null || true
	ip netns exec peer2 ${OVPN_CLI} del_peer tun2 10 2>/dev/null || true

	# close any active socket
	killall $(basename ${OVPN_CLI}) 2>/dev/null || true

	ip netns exec peer1 ${OVPN_CLI} new_peer tun1 ${dev1} 1 10 ${raddr4_peer2} 1 ${raddr4_peer1} 1
	ip netns exec peer1 ${OVPN_CLI} new_key tun1 1 1 0 ${ALG} 0 data64.key
	ip netns exec peer2 ${OVPN_CLI} new_peer tun2 ${dev2} 10 1 ${raddr4_peer1} 1 ${raddr4_peer2} 1
	ip netns exec peer2 ${OVPN_CLI} new_key tun2 10 1 0 ${ALG} 1 data64.key

	ip netns exec peer1 ${OVPN_CLI} set_peer tun1 1 60 120
	ip netns exec peer2 ${OVPN_CLI} set_peer tun2 10 60 120

	timeout 2 ip netns exec peer1 tcpdump -i veth1 "${PROTO,,}" and host ${raddr4_peer2} \
		and port 1 -n -q > /tmp/ovpn-bind1.log &
	tcpdump1_pid=$!
	timeout 2 ip netns exec peer1 tcpdump -i veth2 "${PROTO,,}" and host ${raddr4_peer2} \
		and port 1 -n -q > /tmp/ovpn-bind2.log &
	tcpdump2_pid=$!
	sleep 0.5

	ip netns exec peer1 ping -qfc 50 -w 1 5.5.5.2

	wait ${tcpdump1_pid} || true
	wait ${tcpdump2_pid} || true
}

if [ "${BIND_TYPE}" == "DEV" ]; then
	run_bind_test veth1 any 10.10.10.2 10.10.10.1
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -eq 0 ]

	run_bind_test veth2 any 20.20.20.2 20.20.20.1
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -eq 0 ]

	run_bind_test any veth1 10.10.10.2 10.10.10.1
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -eq 0 ]

	run_bind_test any veth2 20.20.20.2 20.20.20.1
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -eq 0 ]
else
	run_bind_test any any 10.10.10.2 10.10.10.1
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -eq 0 ]

	run_bind_test any any 20.20.20.2 20.20.20.1
	[ "$(grep -c -i udp /tmp/ovpn-bind2.log)" -ge 100 ]
	[ "$(grep -c -i udp /tmp/ovpn-bind1.log)" -eq 0 ]
fi

cleanup

modprobe -r ovpn || true

