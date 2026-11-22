#!/bin/bash
# perf annotate instruction stats test
# SPDX-License-Identifier: GPL-2.0

set -e

shelldir=$(dirname "$0")
. "${shelldir}"/lib/perf_has_symbol.sh

skip_test_missing_symbol datasym

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfone=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perftwo=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
perfout2=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
perferr=$(mktemp /tmp/__perf_test.perf.err.XXXXX)

cleanup()
{
	rm -f "$perfdata" "$perfone" "$perftwo" "$perfout" "$perfout2" "$perferr"
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

if ! perf record -o "$perfone" -e cpu-clock -- perf test -w datasym \
		> /dev/null 2>&1 ||
   ! perf record -o "$perftwo" -e cpu-clock -e task-clock -- \
		perf test -w datasym > /dev/null 2>&1; then
	echo "FAIL: perf record"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type --insn-stat -i "$perfone" --stdio \
		--percent-limit 1 datasym > "$perfout"; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type --insn-stat -i "$perftwo" --stdio \
		--percent-limit 1 datasym > "$perfout2"; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

# The instruction stats are accounted while the instructions are
# displayed; printing them before the display loop shows zeros.  A single
# positive total would also accept a stale first event's total, so check
# that the section is printed once, after the annotations.
if ! awk '
	/^Annotate Instruction stats/ { sections++ }
	sections && /^[[:space:]]*[0-9]+\.[0-9]+ : / { late = 1 }
	END { exit !(sections == 1 && !late) }
' "$perfout2"; then
	echo "FAIL: instruction stats not printed once after the annotations"
	tail -n 30 "$perfout2"
	cleanup
	exit 1
fi

# Each event annotates the same instructions, so the two-event total is
# twice the one-event total.
one_total=$(awk '/^total / { t = $2; sub(/,/, "", t); print t; exit }' \
	    "$perfout")
two_total=$(awk '/^total / { t = $2; sub(/,/, "", t); print t; exit }' \
	    "$perfout2")
if [ -z "$one_total" ] || [ -z "$two_total" ] ||
   [ "$two_total" -le 0 ] || [ "$two_total" -ne $((2 * one_total)) ]; then
	echo "FAIL: instruction stats do not count every event" \
	     "(one event: ${one_total:-none}, two events: ${two_total:-none})"
	tail -n 30 "$perfout2"
	cleanup
	exit 1
fi

# The totals are the sums of the per-instruction rows.
if ! awk '
	/^total / {
		t = $2; o = $4; b = $7
		sub(/,/, "", t); sub(/,/, "", o); sub(/,/, "", b)
		total = t; ok = o; bad = b; next
	}
	/^  .*: +[0-9]+ +[0-9]+$/ { good += $(NF - 1); bads += $NF }
	END {
		exit !(total > 0 && total == good + bads &&
		       ok == good && bad == bads)
	}
' "$perfout2"; then
	echo "FAIL: instruction stats do not add up"
	tail -n 30 "$perfout2"
	cleanup
	exit 1
fi

# No point in recording the data type samples if the PMU cannot do memory
# events.
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

# The data type stats are only printed with --data-type, and must be
# accounted like the instruction stats above.
if ! perf annotate --data-type --type-stat -i "$perfdata" --stdio \
		> "$perfout"; then
	echo "FAIL: perf annotate --data-type --type-stat"
	cleanup
	exit 1
fi

if ! awk '
	/^Annotate data type stats:/ { sections++ }
	sections && /^Annotate type: / { late = 1 }
	END { exit !(sections == 1 && !late) }
' "$perfout"; then
	echo "FAIL: data type stats not printed once after the annotations"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

# The total is the sum of the bad counters and of the good ones.
if ! awk '
	/^Annotate data type stats:/ { stats = 1; next }
	!stats { next }
	/^total / {
		t = $2; o = $4; b = $7
		sub(/,/, "", t); sub(/,/, "", o); sub(/,/, "", b)
		total = t; ok = o; bad = b; next
	}
	/^[[:space:]]*[0-9]+ : / && $3 != "insn_track" { fields += $1 }
	END { exit !(total > 0 && total == ok + bad && bad == fields) }
' "$perfout"; then
	echo "FAIL: data type stats do not add up"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

if ! perf annotate --type-stat -i "$perfdata" --stdio > /dev/null 2> "$perferr" ||
   ! grep -Fxq "Warning: --type-stat needs --data-type" "$perferr"; then
	echo "FAIL: missing --type-stat warning"
	tail -n 30 "$perferr"
	cleanup
	exit 1
fi

echo "instruction stats accounted"
cleanup
