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

# Exercise large TCP flow on 4->6 path to cover pre-fragmentation behavior
RET=0
ipxlat_run_iperf "$NS6" "$NS4" "$IPXLAT_V4_REMOTE" 5301 -n 8M
check_err $? "large tcp 4->6 failed"
log_test "large tcp 4->6"

# Exercise large UDP flow on 4->6 path to cover pre-fragmentation behavior
RET=0
ipxlat_run_iperf "$NS6" "$NS4" "$IPXLAT_V4_REMOTE" 5302 -u -b 20M -t 2 -l 1400
check_err $? "large udp 4->6 failed"
log_test "large udp 4->6"

# Exercise large TCP flow on 6->4 path to cover
# fragmentation-sensitive translation
RET=0
ipxlat_run_iperf "$NS4" "$NS6" "$IPXLAT_V6_NS4" 5303 \
	-B "$IPXLAT_V6_NS6_SRC" -n 8M
check_err $? "large tcp 6->4 failed"
log_test "large tcp 6->4"

# Exercise large UDP flow on 6->4 path to cover
# fragmentation-sensitive translation
RET=0
ipxlat_run_iperf "$NS4" "$NS6" "$IPXLAT_V6_NS4" 5304 \
	-B "$IPXLAT_V6_NS6_SRC" -u -b 20M -t 2 -l 1400
check_err $? "large udp 6->4 failed"
log_test "large udp 6->4"

# Send oversized IPv4 ICMP Echo with DF disabled (source fragmentation allowed)
# and verify translator drops fragmented ICMPv4 input (no translated ICMPv6
# Echo seen in NS6)
RET=0
ipxlat_capture_pkts "$NS6" "icmp6 and ip6[40] == 128" 0 5 \
	ip netns exec "$NS4" bash -c \
	"ping -M \"dont\" -s 2000 -c 1 -W 1 \"$IPXLAT_V4_REMOTE\" \
	>/dev/null 2>&1 || test \$? -eq 1"
check_err $? "fragmented icmp 4->6 should be dropped"
log_test "drop fragmented icmp 4->6"

# Send oversized IPv6 ICMP echo request and verify translator drops fragmented
# ICMPv6 input (no translated ICMPv4 Echo seen in NS4)
RET=0
ipxlat_capture_pkts "$NS4" "icmp and icmp[0] == 8" 0 5 \
	ip netns exec "$NS6" bash -c \
	"ping -6 -s 2000 -c 1 -W 1 -I \"$IPXLAT_V6_NS6_SRC\" \
	\"$IPXLAT_V6_NS4\" >/dev/null 2>&1 || test \$? -eq 1"
check_err $? "fragmented icmp 6->4 should be dropped"
log_test "drop fragmented icmp 6->4"

exit "$EXIT_STATUS"
