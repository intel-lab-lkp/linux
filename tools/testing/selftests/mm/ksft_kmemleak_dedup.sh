#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify that kmemleak's verbose scan output deduplicates leaks that share
# the same allocation backtrace. The kmemleak-test module leaks 10 list
# entries from a single kzalloc() call site, so they share one stackdepot
# trace_handle. With dedup, only one "unreferenced object" line should be
# printed for that backtrace per scan, while the per-scan leak counter
# still accounts for every object.
#
# The expected output is something like:
#   PASS: kmemleak verbose output deduplicated (11 printed for 61 leaks)
#
# Author: Breno Leitao <leitao@debian.org>

ksft_skip=4
KMEMLEAK=/sys/kernel/debug/kmemleak
VERBOSE_PARAM=/sys/module/kmemleak/parameters/verbose
MODULE=kmemleak-test

skip() {
	echo "SKIP: $*"
	exit $ksft_skip
}

fail() {
	echo "FAIL: $*"
	exit 1
}

[ "$(id -u)" -eq 0 ] || skip "must run as root"
[ -r "$KMEMLEAK" ] || skip "no kmemleak debugfs (CONFIG_DEBUG_KMEMLEAK)"
[ -w "$VERBOSE_PARAM" ] || skip "kmemleak verbose param missing"
modinfo "$MODULE" >/dev/null 2>&1 ||
	skip "$MODULE not built (CONFIG_SAMPLE_KMEMLEAK)"

# kmemleak can be present but disabled at runtime (boot arg kmemleak=off,
# or it self-disabled after an internal error). In that state writes other
# than "clear" return EPERM, so probe once and skip if so.
if ! echo scan > "$KMEMLEAK" 2>/dev/null; then
	skip "kmemleak is disabled (check dmesg or kmemleak= boot arg)"
fi

prev_verbose=$(cat "$VERBOSE_PARAM")
cleanup() {
	echo "$prev_verbose" > "$VERBOSE_PARAM" 2>/dev/null
	rmmod "$MODULE" 2>/dev/null
}
trap cleanup EXIT

echo 1 > "$VERBOSE_PARAM"

# Drain the existing leak set so the next scan only reports our objects.
echo clear > "$KMEMLEAK"

modprobe "$MODULE" || fail "failed to load $MODULE"
# Removing the module orphans the list elements without freeing them.
rmmod "$MODULE"    || fail "failed to unload $MODULE"

# Two scans: kmemleak requires the object to survive a full scan cycle
# before it is reported as unreferenced.
dmesg -C >/dev/null
echo scan > "$KMEMLEAK"; sleep 6
echo scan > "$KMEMLEAK"; sleep 6

log=$(dmesg)

new_leaks=$(echo "$log" |
	sed -n 's/.*kmemleak: \([0-9]\+\) new suspected.*/\1/p' | tail -1)
[ -n "$new_leaks" ] || fail "no 'new suspected memory leaks' line found"

# Count "unreferenced object" lines emitted in verbose output.
printed=$(echo "$log" | grep -c 'kmemleak: unreferenced object')

echo "new_leaks=$new_leaks printed=$printed"

# The kzalloc(sizeof(*elem)) loop alone contributes 10 leaks sharing one
# backtrace, so without dedup printed >= 10. With dedup the printed count
# must be strictly less than the reported leak total.
[ "$new_leaks" -ge 10 ] || fail "expected >=10 new leaks, got $new_leaks"
[ "$printed"   -lt "$new_leaks" ] || \
	fail "no dedup: printed=$printed new_leaks=$new_leaks"

echo "PASS: kmemleak verbose output deduplicated" \
	"($printed printed for $new_leaks leaks)"
exit 0
