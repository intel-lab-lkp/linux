#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# +--------------------+
# | H1                 |
# |                    |
# |              $h1 + |
# |     192.0.2.2/28 | |
# | 2001:db8:1::2/64 | |
# |                  | |
# +------------------|-+
#                    |
# +------------------|-----------------------+  +----------------------+
# | SW               |                       |  |                      |
# | (netns ns-r1)    |                       |  | + $neigh1            |
# |             $rp1 +                       |  |   192.0.2.18/28      |
# |     192.0.2.1/28                         |  |   2001:db8:2::2/64   |
# | 2001:db8:1::1/64     + vip               |  |                      |
# |                        198.51.100.0/24   |  +----------------------+
# |                        2001:db8:3::/64   |  |                      |
# |                                          |  | + $neigh2            |
# |                                          |  |   192.0.2.19/28      |
# |                                          |  |   2001:db8:2::3/64   |
# |                                          |  |                      |
# +------------------------------------------+  +----------------------+

ALL_TESTS="multipath_test"
NUM_NETIFS=2
source lib.sh

ns_create()
{
	local ns="$1"

	ip netns add $ns

	in_ns $ns ip link set dev lo up
	in_ns $ns forwarding_enable
}

ns_destroy()
{
	local ns="$1"

	ip netns del $ns &> /dev/null
}

h1_create()
{
	ip link set dev $h1 up
	ip address add 192.0.2.2/28 dev $h1
	ip address add 2001:db8:1::2/64 dev $h1

	ethtool -K $h1 tcp-segmentation-offload off
}

h1_destroy()
{
	ethtool -K $h1 tcp-segmentation-offload on

	ip address del 2001:db8:1::2/64 dev $h1
	ip address del 192.0.2.2/28 dev $h1
	ip link set dev $h1 down
}

router_create()
{
	ns="ns-r1"
	dummy="dum1"

	echo 20000 > /sys/class/net/$rp1/gro_flush_timeout
	echo 1 > /sys/class/net/$rp1/napi_defer_hard_irqs
	ethtool -K $rp1 generic-receive-offload on

	ns_create $ns
	ip link set dev $rp1 netns $ns

	ip -n $ns link set dev $rp1 up
	ip -n $ns address add dev $rp1 192.0.2.1/28

	ip -n $ns link add name $dummy up type dummy
	ip -n $ns address add 192.0.2.17/28 dev $dummy
	ip -n $ns address add 2001:db8:2::1/64 dev $dummy

	ip -n $ns neigh add 192.0.2.18 lladdr 00:11:22:33:44:55 nud perm dev $dummy
	ip -n $ns neigh add 192.0.2.19 lladdr 00:aa:bb:cc:dd:ee nud perm dev $dummy
	ip -n $ns neigh add 2001:db8:2::2 lladdr 00:11:22:33:44:55 nud perm dev $dummy
	ip -n $ns neigh add 2001:db8:2::3 lladdr 00:aa:bb:cc:dd:ee nud perm dev $dummy

	ip -n $ns route add 198.51.100.0/24 \
		nexthop via 192.0.2.18 \
		nexthop via 192.0.2.19

	ip -n $ns route add 2001:db8:3::/64 \
		nexthop via 2001:db8:2::2 \
		nexthop via 2001:db8:2::3
}

router_destroy()
{
	ns="ns-r1"
	dummy="dum1"

	ip -n $ns link del dev $dummy

	ip -n $ns address del dev $rp1 192.0.2.1/28
	ip -n $ns link set dev $rp1 down
	ip -n $ns link set dev $rp1 netns 1

	ns_destroy $ns

	echo 0 > /sys/class/net/$rp1/gro_flush_timeout
	echo 0 > /sys/class/net/$rp1/napi_defer_hard_irqs
	ethtool -K $rp1 generic-receive-offload off
}

perf_stat()
{
	local tracepoint="$1"
	local cmd="$2"
	local out="$3"

	perf stat -o $out -j $tracepoint -a &
	perf_stat_pid=$!

	$cmd

	kill -SIGINT $perf_stat_pid && wait $perf_stat_pid
}

perf_evaluate()
{
	local desc="$1"
	local expected=$2
	local perf_output="$3"

	RET=0
	measured=$(tail -n 1 $perf_output | jq '.["counter-value"] | tonumber | floor')

	diff=$(echo $expected - $measured | bc -l)
	diff=${diff#-}

	test "$(echo "$diff / $expected > 0.15" | bc -l)" -eq 0
	check_err $? "Too large discrepancy between expected and measured fib lookup counts"
	log_test "$desc"
	log_info "Expected count $expected Measured count $measured"
}

run_test_and_check_tpv4()
{
	local src_ip="$1"
	local dst_ip="$2"
	local t0_rp1 t1_rp1
	local expected

	# Transmit multiple flows from h1 to vip and check that fib lookup
	# tracepoint is hit for each packet.
	in_ns ns-r1 sysctl_set net.ipv4.fib_multipath_hash_policy 1

	t0_rp1=$(in_ns ns-r1 link_stats_rx_packets_get $rp1)

	smac=$(mac_get $h1)
	dmac=$(in_ns ns-r1 mac_get $rp1)
	cmd="$MZ $h1 -q -p 64 -a $smac -b $dmac -A $src_ip -B $dst_ip
		-t udp 'sp=1024,dp=1024-65535'"

	perf_output=$(mktemp)
	perf_stat "-e fib:fib_table_lookup" "$cmd" "$perf_output"

	t1_rp1=$(in_ns ns-r1 link_stats_rx_packets_get $rp1)
	expected="$(echo "$t1_rp1 - $t0_rp1" | bc -l)"

	perf_evaluate "IPv4 multipath: fib_table_lookup" $expected "$perf_output"
	rm $perf_output

	in_ns ns-r1 sysctl_restore net.ipv4.fib_multipath_hash_policy
}

run_test_and_check_tpv6()
{
	local src_ip="$1"
	local dst_ip="$2"
	local t0_rp1 t1_rp1
	local expected

	# Transmit multiple flows from h1 to vip and check that fib lookup
	# tracepoint is hit for each packet.
	in_ns ns-r1 sysctl_set net.ipv4.fib_multipath_hash_policy 1

	t0_rp1=$(in_ns ns-r1 link_stats_rx_packets_get $rp1)

	smac=$(mac_get $h1)
	dmac=$(in_ns ns-r1 mac_get $rp1)
	cmd="$MZ $h1 -6 -q -p 64 -a $smac -b $dmac -A $src_ip -B $dst_ip
		-t udp 'sp=1024,dp=1024-65535'"

	perf_output=$(mktemp)
	perf_stat "-e fib6:fib6_table_lookup" "$cmd" "$perf_output"

	# fib6_table_lookup is called twice for each packet, for
	# RT6_TABLE_LOCAL and RT6_TABLE_MAIN
	t1_rp1=$(in_ns ns-r1 link_stats_rx_packets_get $rp1)
	expected="$(echo "2*($t1_rp1 - $t0_rp1)" | bc -l)"

	perf_evaluate "IPv6 multipath: fib6_table_lookup" $expected "$perf_output"
	rm $perf_output

	in_ns ns-r1 sysctl_restore net.ipv4.fib_multipath_hash_policy
}

multipath_test()
{
	log_info "Running multipath tests"
	run_test_and_check_tpv4 "192.0.2.2" "198.51.100.1"
	run_test_and_check_tpv6 "2001:db8:1::2" "2001:db8:3::1"
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	rp1=${NETIFS[p2]}


	h1_create
	router_create
}

setup_wait()
{
	h1=${NETIFS[p1]}
	rp1=${NETIFS[p2]}

	setup_wait_dev $h1
	in_ns ns-r1 setup_wait_dev $rp1

	# Make sure links are ready.
	sleep $WAIT_TIME
}

cleanup()
{
	pre_cleanup

	router_destroy
	h1_destroy
}

trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit $EXIT_STATUS
