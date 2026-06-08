#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# This test is for checking IPv6 jumbogram passthrough through high MTUs.
#
# The test uses three namespaces: A client namespace, a server namespace and a
# router namespace that forwards packets between the client and the server.
#
# +------------------------------------+
# | NS_CLIENT                          |
# |            veth$CLIENT             |
# |                 +                  |
# +-----------------|------------------+
#                   |
# +-----------------|------------------+
# | NS_ROUTER       +                  |
# |         veth$ROUTER_CLIENT         |
# |                                    |
# |         veth$ROUTER_SERVER         |
# |                 +                  |
# +-----------------|------------------+
#                   |
# +-----------------|------------------+
# | NS_SERVER       +                  |
# |            veth$SERVER             |
# |                                    |
# +------------------------------------+

source lib.sh

# All the tests in this script. Can be overridden with -t option.
TESTS="
	test_mtu_low_high
	test_mtu_high_low
	test_mtu_high_medium
	test_mtu_high_high
	test_mtu_probe
	test_gso
	test_fastopen
"
VERBOSE=0

declare NS_CLIENT
declare NS_ROUTER
declare NS_SERVER

readonly CLIENT=1
readonly ROUTER_CLIENT=2
readonly ROUTER_SERVER=3
readonly SERVER=4

readonly CLIENT_ADDR="2001:db8:$CLIENT::$CLIENT"
readonly ROUTER_CLIENT_ADDR="2001:db8:$CLIENT::$ROUTER_CLIENT"
readonly ROUTER_SERVER_ADDR="2001:db8:$SERVER::$ROUTER_SERVER"
readonly SERVER_ADDR="2001:db8:$SERVER::$SERVER"

readonly PORT=8000

readonly MTU=500000
# Leave enough space for headers.
readonly PACKET_SIZE=$((MTU - 100))
readonly PACKET_COUNT=30

################################################################################
# Utilities

run_cmd()
{
	local out
	if ((VERBOSE)); then
		echo "COMMAND: $*"
		out="$("$@")"
	else
		out="$("$@" 2>/dev/null)"
	fi

	local rc="$?"
	if ((VERBOSE)) && [[ -n "$out" ]]; then
		echo "    $out"
	fi

	return "$rc"
}

################################################################################
# Setup

setup()
{
	setup_ns NS_CLIENT NS_ROUTER NS_SERVER

	# Connect the namespaces with veth pairs.
	run_cmd ip link add \
		name "veth$CLIENT" netns "$NS_CLIENT" type veth peer \
		name "veth$ROUTER_CLIENT" netns "$NS_ROUTER"

	run_cmd ip link add \
		name "veth$SERVER" netns "$NS_SERVER" type veth peer \
		name "veth$ROUTER_SERVER" netns "$NS_ROUTER"

	run_cmd ip -n "$NS_CLIENT" link set dev "veth$CLIENT" up
	run_cmd ip -n "$NS_ROUTER" link set dev "veth$ROUTER_CLIENT" up
	run_cmd ip -n "$NS_ROUTER" link set dev "veth$ROUTER_SERVER" up
	run_cmd ip -n "$NS_SERVER" link set dev "veth$SERVER" up

	run_cmd ip -n "$NS_CLIENT" addr add dev \
		"veth$CLIENT" "$CLIENT_ADDR/64" nodad
	run_cmd ip -n "$NS_ROUTER" addr add dev \
		"veth$ROUTER_CLIENT" "$ROUTER_CLIENT_ADDR/64" nodad
	run_cmd ip -n "$NS_ROUTER" addr add dev \
		"veth$ROUTER_SERVER" "$ROUTER_SERVER_ADDR/64" nodad
	run_cmd ip -n "$NS_SERVER" addr add dev \
		"veth$SERVER" "$SERVER_ADDR/64" nodad

	# Set up forwarding through NS_ROUTER.
	run_cmd ip netns exec "$NS_ROUTER" \
		sysctl -wq "net.ipv6.conf.all.forwarding=1"
	run_cmd ip netns exec "$NS_ROUTER" \
		sysctl -wq "net.ipv6.conf.veth$ROUTER_CLIENT.forwarding=1"
	run_cmd ip netns exec "$NS_ROUTER" \
		sysctl -wq "net.ipv6.conf.veth$ROUTER_SERVER.forwarding=1"

	run_cmd ip -n "$NS_CLIENT" -6 route add "$SERVER_ADDR" \
		via "$ROUTER_CLIENT_ADDR" dev "veth$CLIENT"
	run_cmd ip -n "$NS_SERVER" -6 route add "$CLIENT_ADDR" \
		via "$ROUTER_SERVER_ADDR" dev "veth$SERVER"

	# Disable GSO and GRO.
	run_cmd ip netns exec "$NS_CLIENT" ethtool -K "veth$CLIENT" gso off
	run_cmd ip netns exec "$NS_CLIENT" ethtool -K "veth$CLIENT" tso off

	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_SERVER" gso off
	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_SERVER" tso off

	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_CLIENT" gro off
	run_cmd ip netns exec "$NS_SERVER" ethtool -K "veth$SERVER" gro off
}

cleanup()
{
	cleanup_all_ns
}

set_mtus()
{
	local client_mtu="$1"
	local server_mtu="$2"

	run_cmd ip -n "$NS_CLIENT" link set "veth$CLIENT" mtu "$client_mtu"
	run_cmd ip -n "$NS_ROUTER" link set "veth$ROUTER_CLIENT" mtu "$client_mtu"

	run_cmd ip -n "$NS_SERVER" link set "veth$SERVER" mtu "$server_mtu"
	run_cmd ip -n "$NS_ROUTER" link set "veth$ROUTER_SERVER" mtu "$server_mtu"
}

set_gso_max_size()
{
	local gso_max_size="$1"

	run_cmd ip -n "$NS_CLIENT" link set dev "veth$CLIENT" gso_max_size "$gso_max_size"
	run_cmd ip netns exec "$NS_CLIENT" ethtool -K "veth$CLIENT" gso on
	run_cmd ip netns exec "$NS_CLIENT" ethtool -K "veth$CLIENT" tso on

	run_cmd ip -n "$NS_ROUTER" link set dev "veth$ROUTER_CLIENT" gso_max_size "$gso_max_size"
	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_CLIENT" gso on
	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_CLIENT" tso on

	run_cmd ip -n "$NS_ROUTER" link set dev "veth$ROUTER_SERVER" gso_max_size "$gso_max_size"
	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_SERVER" gso on
	run_cmd ip netns exec "$NS_ROUTER" ethtool -K "veth$ROUTER_SERVER" tso on

	run_cmd ip -n "$NS_SERVER" link set dev "veth$SERVER" gso_max_size "$gso_max_size"
	run_cmd ip netns exec "$NS_SERVER" ethtool -K "veth$SERVER" gso on
	run_cmd ip netns exec "$NS_SERVER" ethtool -K "veth$SERVER" tso on
}

################################################################################
# Tests

# Attach a BPF program that keeps track of the maximum packet size it observes.
observe_packets_start() {
	run_cmd tc -n "$NS_SERVER" qdisc add dev "veth$SERVER" clsact
	run_cmd tc -n "$NS_SERVER" filter add dev "veth$SERVER" ingress \
   		bpf object-file jumbogram.bpf.o section ingress
}

max_observed_packet_size() {
	bpftool map lookup name max_packet_size key 0 0 0 0 | jq ".value"
}

observe_packets_stop() {
	run_cmd tc -n "$NS_SERVER" filter del dev "veth$SERVER" ingress
	run_cmd tc -n "$NS_SERVER" qdisc del dev "veth$SERVER" clsact
}

# Check jumbograms are received by the server.
check_jumbogram_passthrough()
{
	local success="$1"
	local expected_size="${2-$PACKET_SIZE}"
	local args=("${@:3}")

	# Start the server.
	local server_stdout="$(mktemp server-stdout-XXXXXX)"
	local server_stderr="$(mktemp server-stderr-XXXXXX)"
	ip netns exec "$NS_SERVER" \
		./jumbogram_rx -p "$PORT" -l $((PACKET_SIZE * PACKET_COUNT)) \
		-C 4000 -R 20 "${args[@]}" >"$server_stdout" 2>"$server_stderr" &
	local server_pid="$!"

	# Wait for the server to start listening.
	for i in {1..4}; do
		if grep -q "listening" "$server_stdout"; then
			break
		fi
		sleep 1
	done

	if ! grep -q "listening" "$server_stdout"; then
		check_err 1 "failed to start server"
		kill "$server_pid"
		wait "$server_pid"
		return
	fi

	observe_packets_start

	# Start the client.
	local client_out="$(mktemp client-out-XXXXXX)"
	ip netns exec "$NS_CLIENT" \
		./jumbogram_tx -D "$SERVER_ADDR" -p "$PORT" -M "$PACKET_COUNT" \
		-s "$PACKET_SIZE" "${args[@]}" >"$client_out" 2>&1

	check_err "$?" "$(cat "$client_out")"
	rm "$client_out"

	# Make sure the server received the correct amount of data.
	run_cmd wait "$server_pid"
	check_err "$?" "$(cat "$server_stderr")"
	rm "$server_stderr" "$server_stdout"

	# Check if at least on packet was not segmented by checking the maximum
	# observed packet size. The first packets are always segmented due to
	# the small initial window size.
	local max_size="$(max_observed_packet_size)"
	observe_packets_stop

	((max_size >= expected_size))
	check_err_fail $((success ^ 1)) "$?" \
		"expected >=$expected_size received $max_size; jumbogram passthrough"
}

# If one side has MTU < 65536, the negotiated MSS should be < 65536.
test_mtu_low_high()
{
	set_mtus 1500 "$MTU"
	check_jumbogram_passthrough 0
	log_test "TCP jumbograms over veth" "client:   1500, server: $MTU"
}

test_mtu_high_low()
{
	set_mtus "$MTU" 1500
	check_jumbogram_passthrough 0
	log_test "TCP jumbograms over veth" "client: $MTU, server:   1500"
}

# If both sides have MTU > 65535 but the message size is above the server MTU,
# smaller jumbograms should still be delivered.
test_mtu_high_medium()
{
	local server_mtu=$((MTU / 2))
	set_mtus "$MTU" "$server_mtu"
	check_jumbogram_passthrough 0 "$PACKET_SIZE"
	check_jumbogram_passthrough 1 $((server_mtu - 100))
	log_test "TCP jumbograms over veth" "client: $MTU, server: $server_mtu"
}

# If both ends have MTU > 65535, message-sized jumbograms should pass through.
test_mtu_high_high()
{
	set_mtus "$MTU" "$MTU"
	check_jumbogram_passthrough 1
	log_test "TCP jumbograms over veth" "client: $MTU, server: $MTU"
}

# MTU probing can't currently settle on MSS > 65535 even if the MTUs allow for
# it due to the MTU-search-range upper bound of 65535. At least make sure that
# the MSS reaches close to 65535.
test_mtu_probe()
{
	run_cmd ip netns exec "$NS_CLIENT" \
		sysctl -wq "net.ipv4.tcp_mtu_probing=2"

	set_mtus "$MTU" "$MTU"
	check_jumbogram_passthrough 0 "$PACKET_SIZE"
	check_jumbogram_passthrough 1 49000
	log_test "High MTUs with MTU probing"
}

# If gso_max_size < MTU then GSO shouldn't be used. gso_max_size > MTU is tested
# in big_tcp.sh.
test_gso()
{
	local gso_max_size=$((MTU - 100))

	set_mtus "$MTU" "$MTU"
	set_gso_max_size "$gso_max_size"
	check_jumbogram_passthrough 1
	log_test "High MTUs with lower gso_max_size"
}

# Make sure TCP fastopen works with MTU > 65535.
test_fastopen()
{
	run_cmd ip netns exec "$NS_CLIENT" \
		sysctl -wq "net.ipv4.tcp_fastopen=3"
	run_cmd ip netns exec "$NS_SERVER" \
		sysctl -wq "net.ipv4.tcp_fastopen=3"

	set_mtus "$MTU" "$MTU"
	check_jumbogram_passthrough 1 "$PACKET_SIZE" -f
	# The same cookie as the previous connection should be used.
	check_jumbogram_passthrough 1 "$PACKET_SIZE" -f
	log_test "High MTUs with TCP fastopen"
}

################################################################################
# Usage

usage()
{
	cat <<EOF
usage: ${0##*/} OPTS

        -t <test>   Test(s) to run (default: all)
                    (options: $TESTS)
        -p          Pause on fail
        -v          Verbose mode (show commands and output)
        -h          Show this help message
EOF
}

################################################################################
# Main

while getopts :t:phv o
do
	case "$o" in
		t) TESTS="$OPTARG";;
		p) PAUSE_ON_FAIL=yes;;
		v) VERBOSE=$((VERBOSE + 1));;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

if [[ "$(id -u)" -ne 0 ]]; then
	echo "SKIP: Need root privileges"
	exit "$ksft_skip";
fi

require_command bpftool
require_command ethtool
require_command ip
require_command iptables
require_command jq
require_command tc

# Start clean.
cleanup

trap cleanup EXIT

for t in $TESTS
do
	setup; "$t"; cleanup;
done

exit "$EXIT_STATUS"
