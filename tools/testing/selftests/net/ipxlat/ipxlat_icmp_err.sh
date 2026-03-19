#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
#
# Copyright (C) 2026- Mandelbit SRL
# Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
#
#  Author:	Antonio Quartulli <antonio@mandelbit.com>
#		Daniel Gröber <dxld@darkboxed.org>
#		Ralf Lici <ralf@mandelbit.com>

set -o pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source "$SCRIPT_DIR/ipxlat_lib.sh"

trap ipxlat_cleanup EXIT

ipxlat_setup_env

# Trigger UDP to a closed port from NS4 and capture translated
# ICMPv4 Port Unreachable
RET=0
ipxlat_capture_pkts "$NS4" "icmp and icmp[0] == 3 and icmp[1] == 3" 1 3 \
	ip netns exec "$NS4" bash -c \
	"echo x > /dev/udp/$IPXLAT_V4_REMOTE/9 || true"
check_err $? "icmp-error 4->6 not observed"
log_test "icmp-error xlate 4->6"

# Trigger UDP to a closed port from NS6 and capture translated
# ICMPv6 Port Unreachable
RET=0
ipxlat_capture_pkts "$NS6" "icmp6 and ip6[40] == 1 and ip6[41] == 4" 1 3 \
	ip netns exec "$NS6" bash -c \
	"echo x > /dev/udp/$IPXLAT_V6_NS4/9 || true"
check_err $? "icmp-error 6->4 not observed"
log_test "icmp-error xlate 6->4"

# Send oversized DF IPv4 packet and verify local ICMPv4
# Fragmentation Needed emission
sysctl -qw net.ipv4.conf.ipxl0.accept_local=1
sysctl -qw net.ipv4.conf.all.rp_filter=0
sysctl -qw net.ipv4.conf.default.rp_filter=0
sysctl -qw net.ipv4.conf.ipxl0.rp_filter=0
sleep 2
RET=0
ipxlat_capture_pkts "$NS4" "icmp and icmp[0] == 3 and icmp[1] == 4" 1 3 \
	ip netns exec "$NS4" bash -c \
	"ping -M \"do\" -s 1300 -c 1 -W 1 \"$IPXLAT_V4_REMOTE\" \
	>/dev/null 2>&1 || test \$? -eq 1"
check_err $? "icmpv4 frag-needed emission not observed"
log_test "icmpv4 frag-needed emission"

exit "$EXIT_STATUS"
