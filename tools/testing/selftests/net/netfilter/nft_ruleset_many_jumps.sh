#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

SYSCTL_MAX_JUMPS=32
DEFAULT_SYSCTL=65536

rnd=$(mktemp -u XXXXXXXX)
ns="nft-$rnd"

cleanup() {
        ip netns del $ns 2>/dev/null || true
        rm -f $ruleset
}

trap cleanup EXIT

set_max_jumps()
{
        local max_jumps=$1

        sysctl -w net.netfilter.nf_tables_jumps_max_netns=$max_jumps 2>&1 >/dev/null
        new_value=$(sysctl -n net.netfilter.nf_tables_jumps_max_netns)
}

get_max_jumps()
{
        local init_net_value=$(sysctl -n net.netfilter.nf_tables_jumps_max_netns)
        echo "$init_net_value"
}

load_ruleset()
{
	local ruleset=$1

	jumps=$(head -1 $ruleset | cut -f3 -d' ')

	ip netns exec $ns nft -f $ruleset &> /dev/null
	if [ "$?" -eq 0 ];then
		if [ $jumps -gt $SYSCTL_MAX_JUMPS ];then
			echo "FAIL: $jumps > $SYSCTL_MAX_JUMPS but ruleset loads"
			cat $ruleset > /tmp/ruleset.nft
			exit 1
		fi
		echo "OK: good ruleset with $jumps jump loads as expected"
	else
		if [ $jumps -lt $SYSCTL_MAX_JUMPS ];then
			echo "FAIL: $jumps < $SYSCTL_MAX_JUMPS but ruleset does not load"
			cat $ruleset > /tmp/ruleset.nft
			exit 1
		fi
		echo "OK: bad ruleset with $jumps jumps fails as expected"
	fi
}

load_ruleset_basic()
{
	ruleset=$(mktemp nft-tempXXXXXXXX.nft)
	echo "table ip x {" > $ruleset
	echo "	chain y0 {" >> $ruleset
	echo "		type filter hook input priority 0;" >> $ruleset
	echo "	}" >> $ruleset
	echo "}" >> $ruleset

	ip netns exec $ns nft -f $ruleset &> /dev/null
	if [ "$?" -ne 0 ];then
		echo "FAIL: cannot load basic ruleset"
		exit 1
	fi
}

flush_ruleset()
{
	local ruleset=$1

	ip netns exec $ns nft flush ruleset
	if [ "$?" -ne 0 ];then
		echo "FAIL: cannot flush ruleset"
		cat $ruleset > /tmp/ruleset.nft
		exit 1
	fi
	rm -f $ruleset
}

pre_max_jumps=$(get_max_jumps)
set_max_jumps $SYSCTL_MAX_JUMPS

ip netns add $ns

for ((i=0;i<10;i++))
do
	echo "=== iteration $i ==="
	filename=$(./gen_ruleset_many_jumps)
	load_ruleset $filename
	flush_ruleset $filename
done

echo "Testing abort path with initial table w/o jumps"

for ((i=0;i<10;i++))
do
	echo "=== iteration $i ==="
	load_ruleset_basic
	filename=$(./gen_ruleset_many_jumps fail $SYSCTL_MAX_JUMPS)
	load_ruleset $filename
	filename=$(./gen_ruleset_many_jumps ok $SYSCTL_MAX_JUMPS)
	load_ruleset $filename
	flush_ruleset $filename
done

set_max_jumps $pre_max_jumps
post_max_jumps=$(get_max_jumps)

if [ "$pre_max_jumps" -ne "$post_max_jumps" ];then
	echo "Fail: Does not init default value: $init_net_value"
	exit 1
fi

exit 0
