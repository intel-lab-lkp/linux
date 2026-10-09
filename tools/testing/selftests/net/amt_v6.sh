#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# amt.sh with an IPv6 outer transport. The gateways and the relay speak AMT
# over IPv6, and IPv4 and IPv6 multicast are carried inside. The namespaces,
# roles and inner addresses are those of amt.sh; the gateway<->relay link
# differs, and two more gateways share it:
#
#   LISTENER l_gw -- gw_l [br0: gw_l, amtg] GATEWAY gw_relay 2001:db8:a::1
#                                                        | IPv6 outer
#                                  [amtg2] GATEWAY2 gw2_relay 2001:db8:a::4
#                                  [amtg3] GATEWAY3 gw3_relay 2001:db8:a::6
#                                                        |
#   SOURCE src_relay -- relay_src [amtr] RELAY br_gw 2001:db8:a::2
#                      [br_gw: relay_gw, relay_gw2, relay_gw3] 2001:db8:a::3
#
# The relay's local address is 2001:db8:a::2. The gateways discover it
# through 2001:db8:a::3, so the Relay Advertisement has to come from the
# address the Discovery was sent to (RFC 7450 s5.1.2) and carry ::2. The
# relay has room for two tunnels, amtg's and amtg2's.

# The namespace names are created by setup_ns, RELAY6 is an address.
# shellcheck disable=SC2153
source lib.sh

readonly GW6="2001:db8:a::1"
readonly GW2_6="2001:db8:a::4"
readonly GW3_6="2001:db8:a::6"
readonly RELAY6="2001:db8:a::2"
readonly DISC6="2001:db8:a::3"
SMCDIR=

cleanup()
{
	cleanup_all_ns
	[ -n "$SMCDIR" ] && rm -rf "$SMCDIR"
}

setup_fail()
{
	echo "FAIL: setup failed at line $1" >&2
	exit "$ksft_fail"
}

amt_field()
{
	ip -n "$1" -d -j link show "$2" |
		jq -r ".[0].linkinfo.info_data.$3 // empty"
}

# remote_is <addr> [<ns> <dev>]: the relay address a gateway learned.
remote_is()
{
	[ "$(amt_field "${2:-$GATEWAY}" "${3:-amtg}" remote)" = "$1" ]
}

snmp6()
{
	# shellcheck disable=SC2016
	ip netns exec "$1" awk -v k="$2" '$1 == k { print $2 }' /proc/net/snmp6
}

setup_links()
{
	local ns

	# No DAD: the relay sources its MLD General Query from amtr's
	# link-local address, which must not be tentative when it is sent.
	for ns in "$LISTENER" "$GATEWAY" "$GATEWAY2" "$GATEWAY3" "$RELAY" \
		  "$SOURCE"; do
		ip netns exec "$ns" sysctl -wq \
			net.ipv6.conf.all.accept_dad=0 \
			net.ipv6.conf.default.accept_dad=0
	done

	ip -n "$LISTENER" link add l_gw type veth peer name gw_l \
		netns "$GATEWAY"
	ip -n "$GATEWAY" link add gw_relay type veth peer name relay_gw \
		netns "$RELAY"
	ip -n "$GATEWAY2" link add gw2_relay type veth peer name relay_gw2 \
		netns "$RELAY"
	ip -n "$GATEWAY3" link add gw3_relay type veth peer name relay_gw3 \
		netns "$RELAY"
	ip -n "$RELAY" link add relay_src type veth peer name src_relay \
		netns "$SOURCE"

	ip -n "$GATEWAY" addr add "$GW6/64" dev gw_relay
	ip -n "$GATEWAY" link set gw_relay up
	ip -n "$GATEWAY2" addr add "$GW2_6/64" dev gw2_relay
	ip -n "$GATEWAY2" link set gw2_relay up
	ip -n "$GATEWAY3" addr add "$GW3_6/64" dev gw3_relay
	ip -n "$GATEWAY3" link set gw3_relay up
	ip -n "$RELAY" link add br_gw type bridge
	ip -n "$RELAY" link set relay_gw master br_gw up
	ip -n "$RELAY" link set relay_gw2 master br_gw up
	ip -n "$RELAY" link set relay_gw3 master br_gw up
	ip -n "$RELAY" addr add "$RELAY6/64" dev br_gw
	ip -n "$RELAY" addr add "$DISC6/64" dev br_gw
	ip -n "$RELAY" link set br_gw up
}

# An iproute2 without IPv6 AMT support either rejects the address or puts
# the whole literal into IFLA_AMT_LOCAL_IP, which this kernel refuses and
# an older one reads as an IPv4 address (its first four bytes). A kernel
# without IPv6 AMT support ignores IFLA_AMT_LOCAL_IP6 and asks for a local
# address. Skip in those cases only.
probe_v6_relay()
{
	local err got

	if ! err=$(ip -n "$RELAY" link add amtprobe type amt mode relay \
		   local "$RELAY6" dev br_gw 2>&1); then
		case "$err" in
		*"Local attribute is required"*|*"expected rather than"*|\
		*"IPv6 address in an IPv4 attribute"*|\
		*"IPv6 support is disabled"*)
			echo "SKIP: no IPv6 AMT support: $err"
			exit "$ksft_skip"
			;;
		esac
		echo "FAIL: cannot create an IPv6 relay: $err"
		exit "$ksft_fail"
	fi
	got=$(amt_field "$RELAY" amtprobe local)
	ip -n "$RELAY" link del amtprobe
	if [[ "$got" != *:* ]]; then
		echo "SKIP: iproute2 lacks IPv6 AMT support (read back '$got')"
		exit "$ksft_skip"
	fi
}

setup_topology()
{
	ip -n "$LISTENER" addr add 192.168.0.2/24 dev l_gw
	ip -n "$LISTENER" addr add 2001:db8::2/64 dev l_gw
	ip -n "$LISTENER" link set l_gw up
	ip -n "$LISTENER" route add default via 192.168.0.1 dev l_gw
	ip -n "$LISTENER" route add default via 2001:db8::1 dev l_gw
	ip -n "$LISTENER" addr add 239.0.0.1/32 dev l_gw autojoin
	ip -n "$LISTENER" addr add ff0e::5:6/128 dev l_gw autojoin

	ip -n "$GATEWAY" addr add 192.168.0.1/24 dev gw_l
	ip -n "$GATEWAY" addr add 2001:db8::1/64 dev gw_l
	ip -n "$GATEWAY" link add br0 type bridge
	ip -n "$GATEWAY" link set br0 up
	ip -n "$GATEWAY" link set gw_l master br0 up
	ip -n "$GATEWAY" link add amtg master br0 type amt mode gateway \
		local "$GW6" discovery "$DISC6" dev gw_relay \
		gateway_port 2268 relay_port 2268

	ip -n "$GATEWAY2" link add amtg2 type amt mode gateway \
		local "$GW2_6" discovery "$DISC6" dev gw2_relay \
		gateway_port 2268 relay_port 2268

	ip -n "$RELAY" link add amtr type amt mode relay local "$RELAY6" \
		dev br_gw relay_port 2268 max_tunnels 2
	ip -n "$RELAY" addr add 172.17.0.1/24 dev relay_src
	ip -n "$RELAY" addr add 2001:db8:3::1/64 dev relay_src
	ip -n "$RELAY" link set relay_src up
	ip netns exec "$RELAY" sysctl -wq net.ipv4.ip_forward=1
	ip netns exec "$RELAY" iptables -t mangle -I PREROUTING \
		-d 239.0.0.1 -j TTL --ttl-set 2
	# Only the group: rewriting the hop limit of Neighbour Discovery on
	# the IPv6 outer link would break it.
	ip netns exec "$RELAY" ip6tables -t mangle -I PREROUTING \
		-d ff0e::5:6 -j HL --hl-set 2

	ip -n "$SOURCE" addr add 172.17.0.2/24 dev src_relay
	ip -n "$SOURCE" addr add 2001:db8:3::2/64 dev src_relay
	ip -n "$SOURCE" link set src_relay up
	ip -n "$SOURCE" route add default via 172.17.0.1 dev src_relay
	ip -n "$SOURCE" route add default via 2001:db8:3::1 dev src_relay

	ip -n "$RELAY" link set amtr up
	ip -n "$GATEWAY" link set amtg up
	ip -n "$GATEWAY2" link set amtg2 up

	SMCDIR=$(mktemp -d)
	ip netns exec "$RELAY" smcrouted -P "$SMCDIR/pid" -u "$SMCDIR/sock"
	slowwait 5 test -S "$SMCDIR/sock"
	ip netns exec "$RELAY" smcroutectl -u "$SMCDIR/sock" \
		a relay_src 172.17.0.2 239.0.0.1 amtr
	ip netns exec "$RELAY" smcroutectl -u "$SMCDIR/sock" \
		a relay_src 2001:db8:3::2 ff0e::5:6 amtr
}

test_discovery()
{
	RET=0
	slowwait 10 remote_is "$RELAY6"
	check_err $? "remote is '$(amt_field "$GATEWAY" amtg remote)'"
	log_test "IPv6 discovery through a secondary relay address"
}

# test_forward <ns> <socat address> <port> <group> <source> <description>
test_forward()
{
	local ns=$1 addr=$2 port=$3 grp=$4 src=$5 desc=$6
	local out pid i

	RET=0
	out=$(mktemp)
	ip netns exec "$ns" timeout 20 \
		socat -u "$addr,readbytes=128" - > "$out" &
	pid=$!
	wait_local_port_listen "$ns" "$port" udp
	for i in $(seq 15); do
		ip netns exec "$SOURCE" bash -c \
			"printf '%s %128s' $src | nc -w 1 -u $grp $port"
		grep -q "$src" "$out" && break
	done
	wait "$pid"
	grep -q "$src" "$out"
	check_err $? "nothing from $src reached the listener"
	rm -f "$out"
	log_test "$desc"
}

# The tunnel MTU is the path MTU to the gateway less the outer headers.
# Lower the relay's route MTU to the gateway below the payload: the relay
# must not fragment the AMT datagram, and it must send the payload's
# source a Packet Too Big (RFC 7450 s5.3.3.6).
test_tmtu()
{
	local ptb frag fragfail i

	RET=0
	ip -n "$RELAY" -6 route add "$GW6/128" dev br_gw mtu 1400
	ptb=$(snmp6 "$SOURCE" Icmp6InPktTooBigs)
	frag=$(snmp6 "$RELAY" Ip6FragCreates)
	fragfail=$(snmp6 "$RELAY" Ip6FragFails)
	for i in 1 2 3; do
		ip netns exec "$SOURCE" bash -c \
			"printf '%1352s' x | nc -w 1 -u ff0e::5:6 6000"
	done
	[ "$(snmp6 "$SOURCE" Icmp6InPktTooBigs)" -gt "$ptb" ] ||
		check_err 1 "the source got no Packet Too Big"
	[ "$(snmp6 "$RELAY" Ip6FragCreates)" -eq "$frag" ] ||
		check_err 1 "the relay fragmented an AMT datagram"
	# amt_udp_xmit() clears ignore_df, so an oversized datagram that got
	# past the tunnel MTU check would be refused by IPv6 output and
	# counted as a FragFail rather than a FragCreate.
	[ "$(snmp6 "$RELAY" Ip6FragFails)" -eq "$fragfail" ] ||
		check_err 1 "an oversized AMT datagram reached IPv6 output"
	ip -n "$RELAY" -6 route del "$GW6/128" dev br_gw mtu 1400
	log_test "IPv6 payload above the tunnel MTU"
}

# A second gateway in the same /64, with the same gateway port as the first.
# The relay has to send each gateway its own Membership Query: a query sent
# to the wrong one fails its nonce check, and the intended gateway never
# reports, so the relay never forwards to it.
test_second_gateway()
{
	RET=0
	slowwait 10 remote_is "$RELAY6" "$GATEWAY2" amtg2
	check_err $? "amtg2 remote is '$(amt_field "$GATEWAY2" amtg2 remote)'"
	log_test "second IPv6 gateway on the link discovers the relay"

	test_forward "$GATEWAY2" \
		UDP4-RECV:4000,ip-add-membership=239.0.0.1:amtg2 \
		4000 239.0.0.1 172.17.0.2 \
		"IPv4 multicast to a second gateway in the same /64"
}

# disc_answer <dst> <filter> [<socat options>]: send a Relay Discovery from
# the gateway's namespace to [<dst>]:2268 and succeed if gw_relay sees a
# packet matching the tcpdump <filter> within a few seconds.
disc_answer()
{
	local dst=$1 filter=$2 opts=${3:-} pid i rc log
	local to="UDP6-SENDTO:[$dst]:2268,sourceport=40000"

	log=$(mktemp)
	ip netns exec "$GATEWAY" timeout 6 \
		tcpdump -nni gw_relay -c 1 "$filter" > /dev/null 2> "$log" &
	pid=$!
	# Send nothing before the capture is live, or a missed packet would
	# pass the negative check.
	busywait 5000 grep -q "listening on" "$log"
	# Type 1 (Relay Discovery), version 0, then a nonce.
	for i in 1 2 3; do
		printf '\x01\x00\x00\x00\x12\x34\x56\x78' |
			ip netns exec "$GATEWAY" socat -u - \
			"$to,so-bindtodevice=gw_relay$opts"
		sleep 0.5
	done
	wait "$pid"
	rc=$?
	rm -f "$log"
	return "$rc"
}

# The relay's socket is bound to ::, so it also receives a Discovery sent
# to ff02::1. Answering it from that destination would put a multicast
# source address on the wire (RFC 4291 s2.7), so the relay must not answer
# it at all. The unicast Discovery is the positive control: it shows that
# the probe and the capture work, and it uses the same capture filter.
test_mcast_discovery()
{
	RET=0
	if ! command -v tcpdump > /dev/null; then
		log_test_skip "Discovery to ff02::1 is not answered" \
			"tcpdump not installed"
		return
	fi
	disc_answer "$DISC6" "udp and src port 2268 and dst port 40000"
	check_err $? "no Advertisement for a unicast Discovery"
	disc_answer ff02::1 "udp and src port 2268 and dst port 40000"
	check_fail $? "the relay answered a Discovery sent to ff02::1"
	log_test "Discovery to ff02::1 is not answered"
}

# amtg and amtg2 hold the relay's two tunnels, so amtg3's Request draws an
# ICMPv6 Destination Unreachable (address unreachable). From a link-local
# address it also shows that the relay sends the error through its
# underlying link: icmp6_send() routes it by skb->dev, and the amt device
# would drop it.
test_tunnel_limit()
{
	local f="icmp6 and ip6[40] == 1 and ip6[41] == 3"
	local kind addr pid log

	for kind in global link-local; do
		RET=0
		if ! command -v tcpdump > /dev/null; then
			log_test_skip "tunnel limit: error to a $kind gateway" \
				"tcpdump not installed"
			continue
		fi
		addr=$GW3_6
		# With a scope filter, ip -j prints {} for each address it
		# leaves out, so select the link-local one instead.
		[ "$kind" = link-local ] &&
			addr=$(ip -n "$GATEWAY3" -6 -j addr show dev gw3_relay |
			       jq -r '.[0].addr_info[] |
				      select(.scope == "link") | .local')
		log=$(mktemp)
		ip netns exec "$GATEWAY3" timeout 10 tcpdump --immediate-mode \
			-nni gw3_relay -c 1 "$f and dst host $addr" \
			> /dev/null 2> "$log" &
		pid=$!
		busywait 5000 grep -q "listening on" "$log"
		ip -n "$GATEWAY3" link add amtg3 type amt mode gateway \
			local "$addr" discovery "$DISC6" dev gw3_relay \
			gateway_port 2268 relay_port 2268
		ip -n "$GATEWAY3" link set amtg3 up
		wait "$pid"
		check_err $? "no Destination Unreachable reached amtg3"
		ip -n "$GATEWAY3" link del amtg3
		rm -f "$log"
		log_test "tunnel limit: error to a $kind gateway"
	done
}

# mcast_data <payload>: printf escapes of AMT Multicast Data carrying an
# IPv4 UDP datagram from 172.17.0.2:5000 to 239.0.0.1:4000.
mcast_data()
{
	local ulen=$((8 + ${#1})) out='\x06\x00' sum b

	# IPv4 header checksum: 0x45, the length, TTL 64 and UDP, the addresses
	sum=$((0x4500 + 20 + ulen + 0x4011 + 0xac11 + 0x0002 + 0xef00 + 0x0001))
	while ((sum >> 16)); do
		sum=$(((sum & 0xffff) + (sum >> 16)))
	done
	sum=$((~sum & 0xffff))
	for b in 0x45 0 $(((20 + ulen) >> 8)) $(((20 + ulen) & 0xff)) 0 0 0 0 \
		 64 17 $((sum >> 8)) $((sum & 0xff)) 172 17 0 2 239 0 0 1 \
		 0x13 0x88 0x0f 0xa0 $((ulen >> 8)) $((ulen & 0xff)) 0 0; do
		out+=$(printf '\\x%02x' "$b")
	done
	printf '%s%s' "$out" "$1"
}

# inject_data <payload> [<socat options>]: send AMT Multicast Data from the
# relay's address to amtg until the listener behind it gets <payload>.
inject_data()
{
	local out pid i rc

	out=$(mktemp)
	ip netns exec "$LISTENER" timeout 20 \
		socat -u "UDP4-LISTEN:4000,readbytes=${#1}" - > "$out" &
	pid=$!
	wait_local_port_listen "$LISTENER" 4000 udp
	for i in $(seq 15); do
		printf '%b' "$(mcast_data "$1")" |
			ip netns exec "$RELAY" socat -u - \
			"UDP6-SENDTO:[$GW6]:2268,bind=[$RELAY6]:40001${2:-}"
		grep -q "$1" "$out" && break
		sleep 1
	done
	wait "$pid"
	grep -q "$1" "$out"
	rc=$?
	rm -f "$out"
	return "$rc"
}

# A gateway accepts Multicast Data with a zero UDP checksum, as RFC 7450
# s5.2.3.3 requires (RFC 6936). The relay always sends a checksum, so the
# test sends the message itself, from the relay's address, which is all the
# gateway checks, with socat setting UDP_NO_CHECK6_TX (SOL_UDP 17, option
# 101). The same message with a checksum is the control.
test_zero_data()
{
	RET=0
	inject_data amt-csum
	check_err $? "control: Multicast Data with a checksum did not arrive"
	inject_data amt-zero ",setsockopt-int=17:101:1"
	check_err $? "Multicast Data with a zero checksum did not arrive"
	log_test "a gateway accepts a zero UDP checksum on Multicast Data"
}

# A relay requires the UDP checksum on every message: a Relay Discovery
# with a zero checksum draws no Advertisement, and counts as an error.
test_zero_relay()
{
	local err

	RET=0
	if ! command -v tcpdump > /dev/null; then
		log_test_skip "a relay refuses a zero UDP checksum" \
			"tcpdump not installed"
		return
	fi
	err=$(snmp6 "$RELAY" Udp6InCsumErrors)
	disc_answer "$DISC6" "udp and src port 2268 and dst port 40000" \
		",setsockopt-int=17:101:1"
	check_fail $? "the relay answered a zero-checksum Discovery"
	[ "$(snmp6 "$RELAY" Udp6InCsumErrors)" -gt "$err" ] ||
		check_err 1 "the relay counted no checksum error"
	log_test "a relay refuses a zero UDP checksum"
}

# adv_listen: receive one Relay Discovery on port 2269 of the discovery
# address, where no relay listens, into $ADV_FILE.
adv_listen()
{
	ip netns exec "$RELAY" timeout 15 socat -u \
		"UDP6-RECV:2269,bind=[$DISC6],readbytes=8" - > "$ADV_FILE" &
	ADV_PID=$!
	wait_local_port_listen "$RELAY" 2269 udp
}

# adv_answer <byte> [<socat options>]: answer that Discovery with a Relay
# Advertisement of 2001:db8:a::<byte>.
adv_answer()
{
	local z9='\x00\x00\x00\x00\x00\x00\x00\x00\x00' nonce='' b

	wait "$ADV_PID" || return 1
	# Type 2 and the nonce, the Discovery's bytes 4-7, then the address.
	for b in $(od -An -tx1 -j4 -N4 "$ADV_FILE"); do
		nonce+="\\x$b"
	done
	printf '%b' "\x02\x00\x00\x00$nonce\x20\x01\x0d\xb8\x00\x0a$z9\x$1" |
		ip netns exec "$RELAY" socat -u - \
		"UDP6-SENDTO:[$GW3_6]:2269,bind=[$DISC6]:2269${2:-}"
}

# A gateway accepts a zero UDP checksum on nothing but Multicast Data. The
# relay never sends one, so the test answers amtg3's Discoveries itself.
# amtg3 runs on port 2269, where no relay listens. It must not learn relay
# ::21 from a zero checksum, but learns ::22 from a checksum, the control.
test_zero_adv()
{
	RET=0
	ADV_FILE=$(mktemp)
	adv_listen
	ip -n "$GATEWAY3" link add amtg3 type amt mode gateway local "$GW3_6" \
		discovery "$DISC6" dev gw3_relay gateway_port 2269 \
		relay_port 2269
	ip -n "$GATEWAY3" link set amtg3 up
	adv_answer 21 ",setsockopt-int=17:101:1"
	check_err $? "no Relay Discovery reached port 2269"
	adv_listen
	sleep 1
	[ -z "$(amt_field "$GATEWAY3" amtg3 remote)" ] ||
		check_err 1 "amtg3 learned a relay from a zero checksum"
	adv_answer 22
	check_err $? "no second Relay Discovery reached port 2269"
	slowwait 5 remote_is 2001:db8:a::22 "$GATEWAY3" amtg3
	check_err $? "control: amtg3 did not learn the checksummed relay"
	ip -n "$GATEWAY3" link del amtg3
	rm -f "$ADV_FILE"
	log_test "a gateway refuses a zero UDP checksum on an Advertisement"
}

test_down_up()
{
	RET=0
	ip -n "$GATEWAY" link set amtg down
	remote_is ""
	check_err $? "relay still reported while down"
	ip -n "$GATEWAY" link set amtg up
	slowwait 10 remote_is "$RELAY6"
	check_err $? "relay not rediscovered after up"
	log_test "gateway forgets its IPv6 relay on link down"
}

for cmd in jq socat nc smcrouted smcroutectl iptables ip6tables; do
	require_command "$cmd"
done

trap cleanup EXIT
setup_ns LISTENER GATEWAY GATEWAY2 GATEWAY3 RELAY SOURCE ||
	exit "$ksft_skip"

set -E
trap 'setup_fail $LINENO' ERR
setup_links
trap - ERR
probe_v6_relay
trap 'setup_fail $LINENO' ERR
setup_topology
trap - ERR
set +E

test_discovery
test_forward "$LISTENER" UDP4-LISTEN:4000 4000 239.0.0.1 172.17.0.2 \
	"IPv4 multicast over an IPv6 tunnel"
test_forward "$LISTENER" UDP6-LISTEN:6000 6000 ff0e::5:6 2001:db8:3::2 \
	"IPv6 multicast over an IPv6 tunnel"
test_second_gateway
test_tunnel_limit
test_tmtu
test_zero_data
test_zero_relay
test_zero_adv
test_mcast_discovery
test_down_up

exit "$EXIT_STATUS"
