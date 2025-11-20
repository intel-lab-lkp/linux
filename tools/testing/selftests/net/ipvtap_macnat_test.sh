#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tests for ipvtap in macnat mode

NS_TST0=ipvlan-tst-0
NS_TST1=ipvlan-tst-1
NS_PHY=ipvlan-tst-phy

IP_HOST=172.25.0.1
IP_PHY=172.25.0.2
IP_TST0=172.25.0.10
IP_TST1=172.25.0.30

IP_OK0=("172.25.0.10" "172.25.0.11" "172.25.0.12" "172.25.0.13")
IP6_OK0=("fc00::10" "fc00::11" "fc00::12" "fc00::13" )

IP_OVFL0="172.25.0.14"
IP6_OVFL0="fc00::14"

IP6_HOST=fc00::1
IP6_PHY=fc00::2
IP6_TST0=fc00::10
IP6_TST1=fc00::30

MAC_HOST="92:3a:00:00:00:01"
MAC_PHY="92:3a:00:00:00:02"
MAC_TST0="92:3a:00:00:00:10"
MAC_TST1="92:3a:00:00:00:30"

VETH_HOST=vethtst
VETH_PHY=vethtst.p

#
# The testing environment looks this way:
#
# |------HOST------|     |------PHY-------|
# |      veth<----------------->veth      |
# |------|--|------|     |----------------|
#        |  |
#        |  |            |-----TST0-------|
#        |  |------------|----ipvtap      |
#        |               |----------------|
#        |
#        |               |-----TST1-------|
#        |---------------|----ivtap       |
#                        |----------------|
#
# The macnat mode is for virtual machines, so ipvtap-interface is supposed
# to be used only for traffic monitoring and doesn't have ip-address.
#
# To simulate a virtual machine on ipvtap, we create TAP-interfaces
# in TST environments and assing IP-addresses to them.
# TAP and IPVTAP are connected with simple python script.
#

ns_run() {
	ns=$1
	shift
	if [[ "$ns" == "default" ]]; then
		"$@" >/dev/null
	else
		ip netns exec "$ns" "$@" >/dev/null
	fi
}

configure_ns() {
	local ns=$1
	local n=$2
	local ip=$3
	local ip6=$4
	local mac=$5

	ns_run "$ns" ip link set lo up

	if ! ip link add netns "$ns" name "ipvtap0.$n" link $VETH_HOST \
	    type ipvtap mode l2macnat bridge; then
		exit_error "FAIL: Failed to configure ipvtap link."
	fi
	ns_run "$ns" ip link set "ipvtap0.$n" up

	ns_run "$ns" ip tuntap add mode tap "tap0.$n"
	ns_run "$ns" ip link set dev "tap0.$n" address "$mac"
	# disable dad
	ns_run "$ns" sysctl -w "net/ipv6/conf/tap0.$n/accept_dad"=0
	ns_run "$ns" ip link set "tap0.$n" up
	ns_run "$ns" ip a a "$ip/24" dev "tap0.$n"
	ns_run "$ns" ip a a "$ip6/64" dev "tap0.$n"
}

start_macnat_bridge() {
	local ns=$1
	local n=$2
	ip netns exec "$ns" python3 ipvtap_macnat_bridge.py \
		"tap0.$n" "ipvtap0.$n" &
}

configure_veth() {
	local ns=$1
	local veth=$2
	local ip=$3
	local ip6=$4
	local mac=$5

	ns_run "$ns" ip link set lo up
	ns_run "$ns" ethtool -K "$veth" tx off rx off
	ns_run "$ns" ip link set dev "$veth" address "$mac"
	ns_run "$ns" ip link set "$veth" up
	ns_run "$ns" ip a a "$ip/24" dev "$veth"
	ns_run "$ns" ip a a "$ip6/64" dev "$veth"
}

setup_env() {
	ip netns add $NS_TST0
	ip netns add $NS_TST1
	ip netns add $NS_PHY

	# setup simulated other-host (phy) and host itself
	ip link add $VETH_HOST type veth peer name $VETH_PHY \
	    netns $NS_PHY >/dev/null

	# host config
	configure_veth default $VETH_HOST $IP_HOST $IP6_HOST $MAC_HOST
	configure_veth $NS_PHY $VETH_PHY $IP_PHY $IP6_PHY $MAC_PHY

	# TST namespaces config
	configure_ns $NS_TST0 0 $IP_TST0 $IP6_TST0 $MAC_TST0
	configure_ns $NS_TST1 1 $IP_TST1 $IP6_TST1 $MAC_TST1
}

ping_all() {
	# This will learn MAC/IP addresses on ipvtap
	local ns=$1

	ns_run "$ns" ping -c 1 $IP_TST0
	ns_run "$ns" ping -c 1 $IP6_TST0

	ns_run "$ns" ping -c 1 $IP_TST1
	ns_run "$ns" ping -c 1 $IP6_TST1

	ns_run "$ns" ping -c 1 $IP_HOST
	ns_run "$ns" ping -c 1 $IP6_HOST

	ns_run "$ns" ping -c 1 $IP_PHY
	ns_run "$ns" ping -c 1 $IP6_PHY
}

check_mac_eq() {
	# Ensure IP corresponds to MAC.
	local ns=$1
	local ip=$2
	local mac=$3
	local dev=$4

	if [[ "$ns" == "default" ]]; then
		out=$(
			ip neigh show "$ip" dev "$dev" \
			| grep "$ip" \
			| grep "$mac"
		)
	else
		out=$(
			ip netns exec "$ns" \
			ip neigh show "$ip" dev "$dev" \
			| grep "$ip" \
			| grep "$mac"
		)
	fi

	if [[ $out'X' == "X" ]]; then
		exit_error "FAIL: '$ip' is not '$mac'"
	fi
}

cleanup_env() {
	ip link del $VETH_HOST
	ip netns del $NS_TST0
	ip netns del $NS_TST1
	ip netns del $NS_PHY
}

exit_error() {
	echo "$1"
	exit 1
}

test_check_mac() {
	# All IPs in NS_PHY should have MAC of the host
	check_mac_eq $NS_PHY $IP_TST0 $MAC_HOST $VETH_PHY
	check_mac_eq $NS_PHY $IP6_TST0 $MAC_HOST $VETH_PHY
	check_mac_eq $NS_PHY $IP_TST1 $MAC_HOST $VETH_PHY
	check_mac_eq $NS_PHY $IP6_TST1 $MAC_HOST $VETH_PHY
	check_mac_eq $NS_PHY $IP_HOST $MAC_HOST $VETH_PHY
	check_mac_eq $NS_PHY $IP6_HOST $MAC_HOST $VETH_PHY

	# All IPs in TST0 should have corresponding MAC
	check_mac_eq $NS_TST0 $IP_HOST $MAC_HOST tap0.0
	check_mac_eq $NS_TST0 $IP6_HOST $MAC_HOST tap0.0
	check_mac_eq $NS_TST0 $IP_TST1 $MAC_TST1 tap0.0
	check_mac_eq $NS_TST0 $IP6_TST1 $MAC_TST1 tap0.0
	check_mac_eq $NS_TST0 $IP_PHY $MAC_PHY tap0.0
	check_mac_eq $NS_TST0 $IP6_PHY $MAC_PHY tap0.0

	# All IPs in host should have corresponding MAC
	check_mac_eq default $IP_TST0 $MAC_TST0 $VETH_HOST
	check_mac_eq default $IP6_TST0 $MAC_TST0 $VETH_HOST
	check_mac_eq default $IP_TST1 $MAC_TST1 $VETH_HOST
	check_mac_eq default $IP6_TST1 $MAC_TST1 $VETH_HOST
	check_mac_eq default $IP_PHY $MAC_PHY $VETH_HOST
	check_mac_eq default $IP6_PHY $MAC_PHY $VETH_HOST
}

test_ip_add() {
	# adding IPs to ipvtap should be forbidden and should fail
	if ns_run $NS_TST0 ip a a 172.26.0.1/24 dev ipvtap0.0; then
		exit_error "FAIL: Module allowed to add ip to ipvtap."
	fi

	if ns_run $NS_TST0 ip a a fc01::1/64 dev ipvtap0.0; then
		exit_error "FAIL: Module allowed to add ip6 to ipvtap."
	fi
}

test_ip_overflow() {
	# The ipvtap remembers limited number of addresses on interface.
	# Let's overflow it and check that oldest one doesn't work.

	ns_run $NS_TST0 ip addr flush dev tap0.0

	# Add exactly 4 ip addresses
	for ip in "${IP_OK0[@]}"; do
		ns_run $NS_TST0 ip a a "$ip/24" dev tap0.0
		ns_run $NS_TST0 ping -c 1 $IP_HOST -I "$ip"
	done

	# Initial check that ping works
	if ! ping -c 2 $IP_TST0; then
		exit_error "FAIL: Failed to ping tst0"
	fi

	# Add 1 more ip addresses
	ns_run "$NS_TST0" ip a a $IP_OVFL0/24 dev tap0.0
	ns_run $NS_TST0 ping -c 1 $IP_HOST -I $IP_OVFL0
	# check that ping to oldest one from host fails.
	echo "the next ping should fail:"
	if ping -c 2 $IP_TST0; then
		exit_error "FAIL: IP-0 still exists on interface"
	fi

	# ping host using address-0 and force relearn of IP0.
	# Host should be able ping after that
	ns_run $NS_TST0 ping -c 1 $IP_HOST -I $IP_TST0

	if ! ping -c 2 $IP_TST0; then
		exit_error "FAIL: Failed to ping tst0 at stage 3"
	fi
}

test_ip6_overflow() {
	# The ipvtap stores limited number of addresses on interface.
	# Let's overflow it and check that oldest one doesn't work.

	ns_run $NS_TST0 ip addr flush dev tap0.0

	# Add exactly 4 ip addresses
	for ip6 in "${IP6_OK0[@]}"; do
		ns_run $NS_TST0 ip a a "$ip6/64" dev tap0.0
		ns_run $NS_TST0 ping -c 1 $IP6_HOST -I "$ip6"
	done

	# Initial check that ping6 works
	if ! ping -c 2 $IP6_TST0; then
		exit_error "FAIL: Failed to ping6 tst0"
	fi

	# Add 1 more ip6 addresses
	ns_run $NS_TST0 ip a a $IP6_OVFL0/64 dev tap0.0
	ns_run $NS_TST0 ping -c 1 $IP6_HOST -I $IP6_OVFL0
	# check that ping to oldest one from host fails.
	echo "the next ping should fail:"
	if ping -c 2 $IP6_TST0; then
		exit_error "FAIL: IP6-0 still exists on interface"
	fi

	# ping host using address-0 and force relearn of IP0.
	# Host should be able ping after that
	ns_run $NS_TST0 ping -c 1 $IP6_HOST -I $IP6_TST0
	if ! ping -c 2 $IP6_TST0; then
		exit_error "FAIL: Failed to ping6 tst0 at stage 3"
	fi
}

exec_test() {
	echo "TEST: $2"
	$1
	echo "PASSED: $2"
}

trap cleanup_env EXIT

echo "ipvlan macnat tests"
echo "==================="

modprobe -q tap
modprobe -q ipvlan
modprobe -q ipvtap

setup_env

exec_test test_ip_add "ip add not allowed"

start_macnat_bridge $NS_TST0 0
mb_pid1=$!
start_macnat_bridge $NS_TST1 1
mb_pid2=$!

echo "<<< Preparation: pinging all...."
ping_all default
ping_all $NS_TST0
ping_all $NS_TST1
ping_all $NS_PHY
echo "Finished preparational pinging all. >>>"

exec_test test_check_mac "mac correctness"
exec_test test_ip_overflow "ip learn capacity overflow"
exec_test test_ip6_overflow "ip6 learn capacity overflow"

kill -INT $mb_pid1
kill -INT $mb_pid2
wait $mb_pid1
wait $mb_pid2

echo "All tests passed"
