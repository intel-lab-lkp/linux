#!/bin/bash
# perf data type profiling test for a global variable
# SPDX-License-Identifier: GPL-2.0

set -e

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)

cleanup()
{
	rm -f "$perfdata" "$perfout"
	trap - EXIT TERM INT
}

trap_cleanup()
{
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

if ! perf check feature -q dwarf; then
	echo "Skip: no DWARF (libdw) support"
	cleanup
	exit 2
fi

# 32-bit x86 PIE reaches static globals relative to the GOT base register,
# which the resolver does not track; e_type's low byte is at 16 on LE x86.
perf_bin=$(command -v perf)
if [ "$(od -An -tx1 -j4 -N1 "$perf_bin" | tr -d ' ')" = 01 ] &&
   [ "$(od -An -tx1 -j16 -N1 "$perf_bin" | tr -d ' ')" = 03 ]; then
	echo "Skip: 32-bit PIE uses GOT-relative global accesses"
	cleanup
	exit 2
fi

# No point in recording if the PMU cannot do memory events.
if ! perf mem record -o /dev/null -- true 2>/dev/null; then
	echo "Skip: cannot record memory events on this PMU"
	cleanup
	exit 2
fi

if ! perf mem record -o "$perfdata" -- perf test -w global_data 1 > /dev/null 2>&1; then
	echo "FAIL: perf mem record"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type -i "$perfdata" --stdio \
		--percent-limit 1 > "$perfout"; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

if ! grep -q "# data-type: struct conn" "$perfout"; then
	echo "FAIL: global variable in .bss not resolved"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

echo "global variable resolved"
cleanup
