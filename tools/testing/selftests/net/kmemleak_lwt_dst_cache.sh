#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
#
# Author: Justin Iurman <justin.iurman@uliege.be>
#
# WARNING
# -------
# This script triggers lwt encap use cases, and checks for any dst cache
# reference loops in affected lwt users (see list below) thanks to kmemleak.
# Some configurations are pathological and some others are valid. Overall, we
# don't want this issue to happen, no matter what, so that's why this selftest
# exists. Note that this script will probably crash the kernel if commit
# 986ffb3a57c5 ("net: lwtunnel: fix recursion loops") is not included.
#
# Affected lwt users so far (please update accordingly if needed):
#  - ila_lwt (output only)
#  - ioam6_iptunnel (output only)
#  - rpl_iptunnel (both input and output)
#  - seg6_iptunnel (both input and output)

source lib.sh
KMEMLEAK_PATH="/sys/kernel/debug/kmemleak"

check_compatibility()
{
	setup_ns tmp_node &>/dev/null
	if [ $? != 0 ]; then
		echo "SKIP: Cannot create netns."
		exit $ksft_skip
	fi

	ip link add name veth0 netns $tmp_node type veth \
		peer name veth1 netns $tmp_node &>/dev/null
	local ret=$?

	ip -netns $tmp_node link set veth0 up &>/dev/null
	ret=$((ret + $?))

	ip -netns $tmp_node link set veth1 up &>/dev/null
	ret=$((ret + $?))

	if [ $ret != 0 ]; then
		echo "SKIP: Cannot configure links."
		cleanup_ns $tmp_node
		exit $ksft_skip
	fi

	lsmod 2>/dev/null | grep -q "ila"
	ila_lsmod=$?
	[ $ila_lsmod != 0 ] && modprobe ila &>/dev/null

	ip -netns $tmp_node route add 2001:db8:1::/64 \
		encap ila 1:2:3:4 csum-mode no-action ident-type luid \
			hook-type output \
		dev veth0 &>/dev/null

	ip -netns $tmp_node route add 2001:db8:2::/64 \
		encap ioam6 trace prealloc type 0x800000 ns 0 size 4 \
		dev veth0 &>/dev/null

	ip -netns $tmp_node route add 2001:db8:3::/64 \
		encap rpl segs 2001:db8:3::1 dev veth0 &>/dev/null

	ip -netns $tmp_node route add 2001:db8:4::/64 \
		encap seg6 mode inline segs 2001:db8:4::1 dev veth0 &>/dev/null

	ip -netns $tmp_node -6 route 2>/dev/null | grep -q "encap ila"
	skip_ila=$?

	ip -netns $tmp_node -6 route 2>/dev/null | grep -q "encap ioam6"
	skip_ioam6=$?

	ip -netns $tmp_node -6 route 2>/dev/null | grep -q "encap rpl"
	skip_rpl=$?

	ip -netns $tmp_node -6 route 2>/dev/null | grep -q "encap seg6"
	skip_seg6=$?

	cleanup_ns $tmp_node
}

setup()
{
	setup_ns alpha beta gamma &>/dev/null

	ip link add name veth-alpha netns $alpha type veth \
		peer name veth-betaL netns $beta &>/dev/null

	ip link add name veth-betaR netns $beta type veth \
		peer name veth-gamma netns $gamma &>/dev/null

	ip -netns $alpha link set veth-alpha name veth0 &>/dev/null
	ip -netns $beta link set veth-betaL name veth0 &>/dev/null
	ip -netns $beta link set veth-betaR name veth1 &>/dev/null
	ip -netns $gamma link set veth-gamma name veth0 &>/dev/null

	ip -netns $alpha addr add 2001:db8:1::2/64 dev veth0 &>/dev/null
	ip -netns $alpha link set veth0 up &>/dev/null
	ip -netns $alpha link set lo up &>/dev/null
	ip -netns $alpha route add 2001:db8:2::/64 \
		via 2001:db8:1::1 dev veth0 &>/dev/null

	ip -netns $beta addr add 2001:db8:1::1/64 dev veth0 &>/dev/null
	ip -netns $beta addr add 2001:db8:2::1/64 dev veth1 &>/dev/null
	ip -netns $beta link set veth0 up &>/dev/null
	ip -netns $beta link set veth1 up &>/dev/null
	ip -netns $beta link set lo up &>/dev/null
	ip -netns $beta route del 2001:db8:2::/64 &>/dev/null
	ip -netns $beta route add 2001:db8:2::/64 dev veth1 &>/dev/null
	ip netns exec $beta \
		sysctl -wq net.ipv6.conf.all.forwarding=1 &>/dev/null

	ip -netns $gamma addr add 2001:db8:2::2/64 dev veth0 &>/dev/null
	ip -netns $gamma link set veth0 up &>/dev/null
	ip -netns $gamma link set lo up &>/dev/null
	ip -netns $gamma route add 2001:db8:1::/64 \
		via 2001:db8:2::1 dev veth0 &>/dev/null

	ip netns exec $alpha ping6 -c 5 -W 1 2001:db8:2::2 &>/dev/null
	if [ $? != 0 ]; then
		echo "SKIP: Setup failed."
		exit $ksft_skip
	fi
}

cleanup()
{
	cleanup_ns $alpha $beta $gamma
	[ $ila_lsmod != 0 ] && modprobe -r ila &>/dev/null
	kmemleak_clear
}

name2descr()
{
	if [ "$1" == "ila" ] || [ "$1" == "ioam6" ]; then
		echo "output"
	elif [ "$1" == "rpl" ] || [ "$1" == "seg6" ]; then
		echo "input + output"
	else
		echo ""
	fi
}

log_test_passed()
{
	printf "TEST: %-57s  [ OK ]\n" "$1"
	npassed=$((npassed+1))
}

log_test_skipped()
{
	printf "TEST: %-57s  [SKIP]\n" "$1"
	nskipped=$((nskipped+1))
}

log_test_failed()
{
	printf "TEST: %-57s  [FAIL]\n" "$1"
	nfailed=$((nfailed+1))
}

check_result()
{
	if grep -q "$1" <<< "$2"; then
		log_test_failed "$1 ($(name2descr $1))"
	else
		log_test_passed "$1 ($(name2descr $1))"
	fi
}

kmemleak_clear()
{
	echo clear > "$KMEMLEAK_PATH"
}

kmemleak_scan()
{
	for i in {1..5}; do
		echo scan > "$KMEMLEAK_PATH"
	done
}

kmemleak_result()
{
	local output=$(cat "$KMEMLEAK_PATH")

	[ $skip_ila != 0 ] && log_test_skipped "ila ($(name2descr ila))" \
			   || check_result "ila" "$output"

	[ $skip_ioam6 != 0 ] && log_test_skipped "ioam6 ($(name2descr ioam6))" \
			   || check_result "ioam6" "$output"

	[ $skip_rpl != 0 ] && log_test_skipped "rpl ($(name2descr rpl))" \
			   || check_result "rpl" "$output"

	[ $skip_seg6 != 0 ] && log_test_skipped "seg6 ($(name2descr seg6))" \
			   || check_result "seg6" "$output"
}

run_ila()
{
	if [ $skip_ila != 0 ]; then
		return
	fi

	ip -netns $beta route del 2001:db8:2::/64 &>/dev/null
	ip -netns $beta route add 2001:db8:2:0:0:0:0:2/128 \
		encap ila 2001:db8:2:0 csum-mode no-action ident-type luid \
			hook-type output \
		dev veth1 &>/dev/null

	ip netns exec $beta ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null

	ip -netns $beta route del 2001:db8:2:0:0:0:0:2/128 &>/dev/null
	ip -netns $beta route add 2001:db8:2::/64 dev veth1 &>/dev/null
}

run_ioam6()
{
	if [ $skip_ioam6 != 0 ]; then
		return
	fi

	ip -netns $beta route change 2001:db8:2::/64 \
		encap ioam6 trace prealloc type 0x800000 ns 1 size 4 \
		dev veth1 &>/dev/null

	ip netns exec $beta ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null

	ip -netns $beta route change 2001:db8:2::/64 dev veth1 &>/dev/null
}

run_rpl()
{
	if [ $skip_rpl != 0 ]; then
		return
	fi

	ip -netns $beta route change 2001:db8:2::/64 \
		encap rpl segs 2001:db8:2::2 \
		dev veth1 &>/dev/null

	ip netns exec $alpha ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null
	ip netns exec $beta ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null

	ip -netns $beta route change 2001:db8:2::/64 dev veth1 &>/dev/null
}

run_seg6()
{
	if [ $skip_seg6 != 0 ]; then
		return
	fi

	ip -netns $beta route change 2001:db8:2::/64 \
		encap seg6 mode inline segs 2001:db8:2::2 \
		dev veth1 &>/dev/null

	ip netns exec $alpha ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null
	ip netns exec $beta ping6 -c 2 -W 1 2001:db8:2::2 &>/dev/null

	ip -netns $beta route change 2001:db8:2::/64 dev veth1 &>/dev/null
}

run()
{
	kmemleak_clear

	run_ila
	run_ioam6
	run_rpl
	run_seg6

	kmemleak_scan
	kmemleak_result
}

npassed=0
nskipped=0
nfailed=0

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: Need root privileges."
	exit $ksft_skip
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool."
	exit $ksft_skip
fi

if [ ! -e $KMEMLEAK_PATH ]; then
	echo "SKIP: Kmemleak not available."
	exit $ksft_skip
fi

check_compatibility

trap cleanup EXIT

setup
run

if [ $nfailed != 0 ]; then
	exit $ksft_fail
fi

exit $ksft_pass
