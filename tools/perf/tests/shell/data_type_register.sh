#!/bin/bash
# perf data type profiling tests for register variables
# SPDX-License-Identifier: GPL-2.0

set -e

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
perferr=$(mktemp /tmp/__perf_test.perf.err.XXXXX)

cleanup()
{
	rm -f "$perfdata" "$perfout" "$perferr"
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

# 32-bit x86 passes the struct pointer in a stack slot, so nothing is
# resolved from a register; e_machine's low byte is at 18 on LE x86.
perf_bin=$(command -v perf)
if [ "$(od -An -tx1 -j18 -N1 "$perf_bin" | tr -d ' ')" = 03 ]; then
	echo "Skip: 32-bit x86 keeps the argument in a stack slot"
	cleanup
	exit 2
fi

# No point in recording if the PMU cannot do memory events.
if ! perf mem record -o /dev/null -- true 2>/dev/null; then
	echo "Skip: cannot record memory events on this PMU"
	cleanup
	exit 2
fi

if ! perf mem record -o "$perfdata" -- perf test -w regptr 1 \
		> /dev/null 2> "$perferr"; then
	echo "FAIL: perf mem record"
	cat "$perferr"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type -i "$perfdata" --stdio \
		--percent-limit 1 touch > "$perfout"; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

for member in "+0 (a)" "+0x8 (b)" "+0x10 (c)"; do
	if ! grep -Fq "# data-type: struct node ${member}" "$perfout"; then
		echo "FAIL: member ${member} not resolved from the register"
		grep 'struct node' "$perfout" || true
		cleanup
		exit 1
	fi
done

if ! perf annotate --data-type --type-stat -i "$perfdata" --stdio \
		touch > "$perfout"; then
	echo "FAIL: perf annotate --data-type --type-stat"
	cleanup
	exit 1
fi

if grep -Eq '^[[:space:]]*[0-9]+ : insn_track$' "$perfout"; then
	echo "FAIL: data type resolved by instruction tracking"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

echo "register held access resolved"
cleanup
