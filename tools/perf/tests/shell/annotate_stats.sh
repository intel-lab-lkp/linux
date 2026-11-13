#!/bin/bash
# perf annotate instruction stats test
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

# No point in recording if the PMU cannot do memory events.
if perf mem record -o /dev/null -- true 2>&1 | \
		grep -q "failed: no PMU supports the memory events"; then
	cleanup
	exit 2
fi
if ! perf mem record -o /dev/null -- true 2>/dev/null; then
	echo "Skip: cannot record memory events on this PMU"
	cleanup
	exit 2
fi

if ! perf mem record -o "$perfdata" -- perf test -w datasym > /dev/null 2>&1; then
	echo "FAIL: perf mem record"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type --insn-stat -i "$perfdata" --stdio \
		--percent-limit 1 > "$perfout" 2>/dev/null; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

# The instruction stats are accounted while the instructions are
# displayed; printing them before the display loop shows zeros.
if ! grep -Eq '^total [1-9][0-9]*,' "$perfout"; then
	echo "FAIL: instruction stats not accounted"
	cleanup
	exit 1
fi

echo "instruction stats accounted"
cleanup
