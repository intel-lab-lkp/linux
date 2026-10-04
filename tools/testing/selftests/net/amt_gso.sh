#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Check that an AMT relay forwards a UDP GSO burst (UDP_SEGMENT) to a gateway
# when transmit checksum offload is enabled on the amt device.
#
# With the default features the core segments the burst before it reaches
# amt_dev_xmit(), so that case is the positive control for the harness. With
# "tx on" the unsegmented GSO skb is handed to the driver, which must mark it
# as a UDP tunnel packet before it hands it to the lower device.
#
# There are three network namespaces. The sender runs in the RELAY namespace,
# because a forwarded GRO skb with DF set would be dropped by the multicast
# router before it ever gets to amt.
#
#   LISTENER             GATEWAY                RELAY
#  +---------+        +-------------+       +-----------------+
#  |  l_gw   |--------| gw_l  br0   |       |                 |
#  |         |        |       amtg  |       |  amtr  <- sender|
#  +---------+        |  gw_relay   |-------| relay_gw        |
#                     +-------------+       +-----------------+
#
# amt_gso, built from amt_gso.c, sends, receives and captures. The listener
# reports datagrams by count, length and payload. A capture on amtr shows what
# was handed to amt_dev_xmit(), and one on gw_relay shows what was put on the
# wire.

source lib.sh

AMT_GSO=./amt_gso
GRP4=239.0.0.1
GRP6=ff0e::5:6
SRC4=192.0.2.1
SRC6=2001:db8:3::1
PORT_AMT=2268
SEG=1200
TAIL=100
BURST=8
NUM=100
PROBE_OPTS=(-s 64 -c 1 -t 0 -n 1)

TMPD=$(mktemp -d)

cleanup()
{
	rm -rf "$TMPD"
	cleanup_all_ns
}

trap cleanup EXIT

dev_stat()
{
	ip netns exec "$1" cat "/sys/class/net/$2/statistics/$3"
}

csum_errors()
{
	ip netns exec "$GATEWAY" nstat -asz UdpInCsumErrors |
		awk '$1 == "UdpInCsumErrors" { n = $2 } END { print n + 0 }'
}

field()
{
	local val

	val=$(grep -o " $2=[0-9]*" "$1" | tail -n 1 | cut -d= -f2)
	echo "${val:-0}"
}

skip_all()
{
	log_test_skip "$1"
	exit "$EXIT_STATUS"
}

fail_all()
{
	RET=$ksft_fail retmsg=$2 log_test "$1"
	exit "$EXIT_STATUS"
}

setup_topology()
{
	setup_ns LISTENER GATEWAY RELAY || exit $ksft_skip

	ip link add l_gw netns "$LISTENER" type veth peer name gw_l \
		netns "$GATEWAY"
	ip link add gw_relay netns "$GATEWAY" type veth peer name relay_gw \
		netns "$RELAY"

	ip -n "$LISTENER" link set l_gw up
	ip -n "$LISTENER" addr add 192.168.0.2/24 dev l_gw
	ip -n "$LISTENER" addr add 2001:db8::2/64 dev l_gw nodad
	ip -n "$LISTENER" route add default via 192.168.0.1 dev l_gw
	ip -n "$LISTENER" addr add "$GRP4"/32 dev l_gw autojoin
	ip -n "$LISTENER" addr add "$GRP6"/128 dev l_gw autojoin

	ip -n "$GATEWAY" link set gw_l up
	ip -n "$GATEWAY" link set gw_relay up
	ip -n "$GATEWAY" addr add 192.168.0.1/24 dev gw_l
	ip -n "$GATEWAY" addr add 2001:db8::1/64 dev gw_l nodad
	ip -n "$GATEWAY" addr add 10.0.0.1/24 dev gw_relay
	ip -n "$GATEWAY" link add br0 type bridge
	ip -n "$GATEWAY" link set br0 up
	ip -n "$GATEWAY" link set gw_l master br0
	ip -n "$GATEWAY" link add amtg master br0 type amt mode gateway \
		local 10.0.0.1 discovery 10.0.0.2 dev gw_relay \
		gateway_port $PORT_AMT relay_port $PORT_AMT || exit $ksft_skip

	ip -n "$RELAY" link set relay_gw up
	ip -n "$RELAY" addr add 10.0.0.2/24 dev relay_gw
	ip -n "$RELAY" link add amtr type amt mode relay local 10.0.0.2 \
		dev relay_gw relay_port $PORT_AMT max_tunnels 4 ||
		exit $ksft_skip
	ip -n "$RELAY" addr add "$SRC4"/32 dev amtr
	ip -n "$RELAY" addr add "$SRC6"/128 dev amtr nodad
	ip -n "$RELAY" link set amtr up
	ip -n "$GATEWAY" link set amtg up

	# Segment and checksum in software on the relay's egress, so that the
	# frames on the wire are at most one MTU and carry a final checksum
	# whatever the veth can do.
	ip netns exec "$RELAY" ethtool -K relay_gw tx off >/dev/null ||
		exit $ksft_skip

	AMTR_MTU=$(ip netns exec "$RELAY" cat /sys/class/net/amtr/mtu)
}

# Send one single-datagram probe every second until the listener sees it,
# which means that discovery, request and update are done for this group.
wait_tunnel()
{
	local fam=$1 v6="" grp=$GRP4 src=$SRC4 port=4999 i

	[ "$fam" = 6 ] && { v6=-6; grp=$GRP6; src=$SRC6; port=6999; }

	for i in $(seq 40); do
		rm -f "$TMPD/probe.out"
		ip netns exec "$LISTENER" $AMT_GSO recv $v6 -p $port \
			"${PROBE_OPTS[@]}" -T 1 >"$TMPD/probe.out" &
		local pid=$!
		busywait 5000 grep -q READY "$TMPD/probe.out"
		ip netns exec "$RELAY" $AMT_GSO send $v6 -I amtr -g $grp \
			-b $src -p $port "${PROBE_OPTS[@]}"
		wait $pid && return 0
	done
	return 1
}

# run_burst <4|6> <gso 0|1>: send NUM datagrams and capture. Sets RECV_RC.
run_burst()
{
	local fam=$1 gso=$2 v6="" grp=$GRP4 src=$SRC4 port=4000 cnt=$BURST
	local tail=$TAIL gsoopt="" pid_r pid_a pid_g f

	[ "$fam" = 6 ] && { v6=-6; grp=$GRP6; src=$SRC6; port=6000; }
	if [ "$gso" = 1 ]; then
		gsoopt=-G
	else
		cnt=1
		tail=0
	fi
	local opts=(-s "$SEG" -c "$cnt" -t "$tail" -n "$NUM")

	rm -f "$TMPD"/{recv,amtr,gw}.out

	ip netns exec "$LISTENER" $AMT_GSO recv $v6 -p $port "${opts[@]}" \
		-T 3 -S 15 >"$TMPD/recv.out" &
	pid_r=$!
	ip netns exec "$RELAY" $AMT_GSO sniff -I amtr -p $port -M "$AMTR_MTU" \
		-T 20 >"$TMPD/amtr.out" &
	pid_a=$!
	ip netns exec "$GATEWAY" $AMT_GSO sniff -I gw_relay -p $PORT_AMT \
		-M 1500 -a $v6 -T 20 >"$TMPD/gw.out" &
	pid_g=$!
	for f in recv amtr gw; do
		busywait 5000 grep -q READY "$TMPD/$f.out" ||
			check_err 1 "amt_gso $f did not start"
	done

	DROP0=$(dev_stat "$RELAY" relay_gw tx_dropped)
	TXP0=$(dev_stat "$RELAY" relay_gw tx_packets)
	CSUM0=$(csum_errors)
	GRX0=$(dev_stat "$GATEWAY" amtg rx_packets)
	LRX0=$(dev_stat "$LISTENER" l_gw rx_packets)
	GRXD0=$(dev_stat "$GATEWAY" amtg rx_dropped)

	ip netns exec "$RELAY" $AMT_GSO send $v6 -I amtr -g $grp -b $src \
		-p $port "${opts[@]}" $gsoopt -d 2000

	wait $pid_r
	RECV_RC=$?
	# The receiver is done, so every frame the captures will see is already
	# queued on their sockets. Ask them to drain it and report.
	kill -TERM $pid_a $pid_g
	wait $pid_a
	check_err $? "the capture on amtr failed"
	wait $pid_g
	check_err $? "the capture on gw_relay failed"

	DROPS=$(($(dev_stat "$RELAY" relay_gw tx_dropped) - DROP0))
	TXP=$(($(dev_stat "$RELAY" relay_gw tx_packets) - TXP0))
	CSUMERR=$(($(csum_errors) - CSUM0))
	GRX=$(($(dev_stat "$GATEWAY" amtg rx_packets) - GRX0))
	LRX=$(($(dev_stat "$LISTENER" l_gw rx_packets) - LRX0))
	GRXD=$(($(dev_stat "$GATEWAY" amtg rx_dropped) - GRXD0))
	EXPECT=$((NUM * (cnt + (tail ? 1 : 0))))
}

# run_case <name> <4|6> <tx on|off> <gso 0|1>
run_case()
{
	local name=$1 fam=$2 tx=$3 gso=$4 big gwn stats

	RET=0
	retmsg=
	ip netns exec "$RELAY" ethtool -K amtr tx "$tx" >/dev/null
	if [ "$tx" = on ] && ! ip netns exec "$RELAY" ethtool -k amtr |
	   grep -q '^tx-udp-segmentation: on'; then
		log_test_skip "$name" "amtr cannot take tx-udp-segmentation"
		return
	fi

	run_burst "$fam" "$gso"
	big=$(field "$TMPD/amtr.out" over_mtu)

	gwn=$(field "$TMPD/gw.out" amt_data)
	log_info "$name: $(grep RECV "$TMPD/recv.out")"
	log_info "$name: amtr tx $(grep SNIFF "$TMPD/amtr.out")"
	log_info "$name: wire $(grep SNIFF "$TMPD/gw.out")"
	stats="relay_gw tx +$TXP drop +$DROPS; gw csum_err +$CSUMERR"
	stats="$stats; amtg rx +$GRX drop +$GRXD; listener rx +$LRX"
	log_info "$name: $stats"

	check_err $(($(field "$TMPD/amtr.out" lost) + \
		     $(field "$TMPD/gw.out" lost) != 0)) \
		"a packet capture lost frames, the verdict is unreliable"
	check_err $RECV_RC "listener did not get every datagram intact"
	check_err $((DROPS != 0)) "relay_gw dropped $DROPS packets"
	check_err $((CSUMERR != 0)) "gateway counted $CSUMERR csum errors"
	check_err $((GRXD != 0)) "amtg dropped $GRXD packets"
	check_err $((gwn != EXPECT)) \
		"gateway saw $gwn AMT data messages, expected $EXPECT"
	check_err $(($(field "$TMPD/gw.out" over_mtu) != 0)) \
		"frames larger than the MTU were put on the wire"

	if [ "$tx" = on ] && [ "$gso" = 1 ]; then
		# Without this the case proves nothing about the driver.
		check_err $((big == 0)) "no GSO skb reached amt_dev_xmit()"
	else
		check_err $((big != 0)) \
			"a frame larger than the MTU reached amt_dev_xmit()"
	fi

	log_test "$name"
}

require_command ip
require_command ethtool
require_command nstat
[ -x $AMT_GSO ] || skip_all "amt_gso helper not built"
ip link help 2>&1 | grep -q amt || skip_all "iproute2 without amt support"
# Without AF_PACKET (CONFIG_PACKET, CAP_NET_RAW) there is no verdict.
$AMT_GSO sniff -I lo -p 1 -T 0 >/dev/null 2>&1 ||
	skip_all "AF_PACKET capture not available"

setup_topology

wait_tunnel 4 ||
	fail_all "IPv4 AMT tunnel" "no relayed probe reached the listener"
wait_tunnel 6 ||
	fail_all "IPv6 AMT tunnel" "no relayed probe reached the listener"

run_case "IPv4 UDP_SEGMENT burst, amt tx offload off (control)" 4 off 1
run_case "IPv6 UDP_SEGMENT burst, amt tx offload off (control)" 6 off 1
run_case "IPv4 UDP_SEGMENT burst, amt tx offload on" 4 on 1
run_case "IPv6 UDP_SEGMENT burst, amt tx offload on" 6 on 1
run_case "IPv4 plain datagrams, amt tx offload on" 4 on 0
run_case "IPv6 plain datagrams, amt tx offload on" 6 on 0

exit "$EXIT_STATUS"
