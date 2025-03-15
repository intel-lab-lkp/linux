#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Check conntrack bridge is functional in various vlan setups.
#
# Setup is:
#
# nsclient1 -> nsbr -> nsclient2
# ping nsclient2 from nsclient1, checking that conntrack established
# packets are seen.
#

source lib.sh

if ! nft --version > /dev/null 2>&1;then
	echo "SKIP: Could not run test without nft tool"
	exit $ksft_skip
fi

cleanup() {
	cleanup_all_ns
}

trap cleanup EXIT

setup_ns nsclient1 nsclient2 nsbr

ret=0

add_addr()
{
	ns=$1
	dev=$2
	i=$3

	ip -net "$ns" link set "$dev" up
	ip -net "$ns" addr add "192.168.1.$i/24" dev "$dev"
	ip -net "$ns" addr add "dead:1::$i/64" dev "$dev" nodad
	ip -net "$ns" route add default dev "$dev"
}

del_addr()
{
	ns=$1
	dev=$2
	i=$3

	ip -net "$ns" route del default dev "$dev"
	ip -net "$ns" addr del "dead:1::$i/64" dev "$dev" nodad
	ip -net "$ns" addr del "192.168.1.$i/24" dev "$dev"
	ip -net "$ns" link set "$dev" down
}

send_pings()
{
	for ad in "$@"; do
		if ! ip netns exec "$nsclient1" ping -c 1 -s 962 -q "$ad" >/dev/null; then
			echo "ERROR: netns routing/connectivity broken to $ad" 1>&2
			exit 1
		fi
	done
}

check_counter()
{
	ns=$1
	name=$2
	expect=$3
	local lret=0

	if ! ip netns exec "$ns" nft list counter bridge filter "$name" | grep -q "$expect"; then
		echo "ERROR: counter $name in $ns has unexpected value (expected $expect)" 1>&2
		ip netns exec "$ns" nft list counter bridge filter "$name" 1>&2
		lret=1
	fi
	ip netns exec "$ns" nft reset counters >/dev/null

	return $lret
}

BR=br0
if ! ip -net "$nsbr" link add $BR type bridge; then
	echo "SKIP: Can't create bridge $BR"
	exit $ksft_skip
fi

DEV=veth0
ip link add "$DEV" netns "$nsclient1" type veth peer name eth1 netns "$nsbr"
ip link add "$DEV" netns "$nsclient2" type veth peer name eth2 netns "$nsbr"

ip -net "$nsbr" link set eth1 master $BR up
ip -net "$nsbr" link set eth2 master $BR up
ip -net "$nsbr" link set $BR up

ip netns exec "$nsbr" nft -f - <<EOF
table bridge filter {
	counter established { }
	chain forward {
		type filter hook forward priority 0; policy accept;
		ct state "established" counter name "established"
	}
}
EOF

a=1;
for ns in "$nsclient1" "$nsclient2"; do
	add_addr "$ns" "$DEV" $a
	((a++))
done

send_pings "192.168.1.2" "dead:1::2"
expect="packets 2 bytes 2000"
if ! check_counter "$nsbr" "established" "$expect"; then
	msg+="\nFAIL: without vlan, established packets not seen"
	ret=1
fi

a=1;
for ns in "$nsclient1" "$nsclient2"; do
	del_addr "$ns" "$DEV" $a
	ip -net "$ns" link add link "$DEV" name "$DEV.10" type vlan id 10
	ip -net "$ns" link set "$DEV" up
	add_addr "$ns" "$DEV.10" $a
	((a++))
done

send_pings "192.168.1.2" "dead:1::2"
expect="packets 2 bytes 2000"
if ! check_counter "$nsbr" "established" "$expect"; then
	msg+="\nFAIL: with single vlan, established packets not seen"
	ret=1
fi

a=1;
for ns in "$nsclient1" "$nsclient2"; do
	del_addr "$ns" "$DEV.10" $a
	ip -net "$ns" link add link "$DEV.10" name "$DEV.10.20" type vlan id 20
	ip -net "$ns" link set "$DEV.10" up
	add_addr "$ns" "$DEV.10.20" $a
	((a++))
done

send_pings "192.168.1.2" "dead:1::2"
expect="packets 2 bytes 2008"
if ! check_counter "$nsbr" "established" "$expect"; then
	msg+="\nFAIL: with double q vlan, established packets not seen"
	ret=1
fi

a=1;
for ns in "$nsclient1" "$nsclient2"; do
	del_addr "$ns" "$DEV.10.20" $a
	ip -net "$ns" link del "$DEV.10.20"
	ip -net "$ns" link del "$DEV.10"
	ip -net "$ns" link add link "$DEV" name "$DEV.10" type vlan id 10 protocol 802.1ad
	ip -net "$ns" link add link "$DEV.10" name "$DEV.10.20" type vlan id 20
	ip -net "$ns" link set "$DEV.10" up
	add_addr "$ns" "$DEV.10.20" $a
	((a++))
done

send_pings "192.168.1.2" "dead:1::2"
expect="packets 2 bytes 2008"
if ! check_counter "$nsbr" "established" "$expect"; then
	msg+="\nFAIL: with 802.1ad vlan, established packets not seen "
	ret=1
fi

if [ $ret -eq 0 ];then
	echo "PASS: established packets seen in all cases"
else
	echo -e "$msg"
fi

exit $ret

