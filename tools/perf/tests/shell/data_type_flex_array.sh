#!/bin/bash
# perf data type profiling tests for flexible array members
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

if ! perf mem record -o "$perfdata" -- perf test -w flex_array 1 \
		> /dev/null 2> "$perferr"; then
	echo "FAIL: perf mem record"
	cat "$perferr"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type -i "$perfdata" --stdio \
		--percent-limit 1 > "$perfout"; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

if ! grep -Fq "# data-type: struct flex +0x4 (data)" "$perfout"; then
	echo "FAIL: flexible array member not resolved"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

# The accesses to the flexible array are resolved past the end of the
# struct: they must count for the struct, not only for the member.
if ! perf annotate --data-type -i "$perfdata" --stdio > "$perfout"; then
	echo "FAIL: perf annotate --data-type"
	cleanup
	exit 1
fi

if ! awk '
	$1 + 0 == 100.00 && /struct flex/ { struct_ok = 1 }
	$1 + 0 > 0 && /char\[\]/ && /data;/ { data_ok = 1 }
	END { exit !(struct_ok && data_ok) }
' <(sed -n "/^Annotate type: 'struct flex' in/,/^Annotate type: /p" "$perfout"); then
	echo "FAIL: --data-type does not count the flexible array accesses"
	tail -n 30 "$perfout"
	cleanup
	exit 1
fi

# Same with the samples and period columns, the other two display modes.
for mode in --show-nr-samples --show-total-period; do
	if ! perf annotate --data-type -i "$perfdata" --stdio "$mode" > "$perfout"; then
		echo "FAIL: perf annotate --data-type $mode"
		cleanup
		exit 1
	fi

	if ! awk '
		$1 + 0 > 0 && /struct flex/ { total = $1 + 0 }
		/int/ && /len;/ { len = $1 + 0 }
		/char\[\]/ && /data;/ { data = $1 + 0 }
		END { exit !(total > 0 && data > 0 && total == data + len) }
	' <(sed -n "/^Annotate type: 'struct flex' in/,/^Annotate type: /p" "$perfout"); then
		echo "FAIL: --data-type $mode does not count the flexible array accesses"
		tail -n 30 "$perfout"
		cleanup
		exit 1
	fi
done

echo "flexible array member resolved and counted"
cleanup
