#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Functional test for kmemleak's N-consecutive-scan leak confirmation
# (the min_unref_scans module parameter).
#
# kmemleak only reports an object once it has stayed unreferenced for
# min_unref_scans consecutive scans. The default of 1 reports on the first
# scan (historical behaviour); higher values filter transient false
# positives where a live object's only reference is briefly invisible to a
# single scan (e.g. an RCU tree update in flight while the scan runs). The
# test loads samples/kmemleak's helper module to create orphan allocations
# and, counting only those orphans (matched by their [kmemleak_test]
# backtrace so unrelated leaks already present on the system are ignored),
# checks that:
#   - min_unref_scans=1 reports them on the first scan,
#   - min_unref_scans=2 reports nothing on the first scan but does on the
#     second,
#   - the parameter reads back what was written.
#
# The "nothing on the first scan" check is the core regression test: with
# min_unref_scans=2 no object can be reported in fewer than two scans. Like
# ksft_kmemleak_dedup.sh, if the module yields no detectable orphan at all
# in the running environment the test skips rather than failing.
#
# Author: Breno Leitao <leitao@debian.org>

ksft_skip=4
KMEMLEAK=/sys/kernel/debug/kmemleak
PARAM=/sys/module/kmemleak/parameters/min_unref_scans
MODULE=kmemleak-test
AGE=6		# seconds; must exceed kmemleak's 5s minimum object age

skip() { echo "SKIP: $*"; exit $ksft_skip; }
fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; exit 0; }

[ "$(id -u)" -eq 0 ] || skip "must run as root"
[ -r "$KMEMLEAK" ] || skip "no kmemleak debugfs (CONFIG_DEBUG_KMEMLEAK)"
[ -w "$PARAM" ] || skip "min_unref_scans module parameter not present"
modinfo "$MODULE" >/dev/null 2>&1 ||
	skip "$MODULE not built (CONFIG_SAMPLE_KMEMLEAK)"

# kmemleak can be present but disabled at runtime (kmemleak=off boot arg,
# or it self-disabled after an internal error); a "scan" then returns
# EPERM. Probe once and skip if so.
echo scan > "$KMEMLEAK" 2>/dev/null ||
	skip "kmemleak is disabled (check dmesg or kmemleak= boot arg)"

prev=$(cat "$PARAM")
# shellcheck disable=SC2317  # invoked indirectly via trap
cleanup() {
	echo "$prev" > "$PARAM" 2>/dev/null		# restore the parameter
	echo scan=on > "$KMEMLEAK" 2>/dev/null		# re-enable auto scan
	rmmod "$MODULE" 2>/dev/null
	echo clear > "$KMEMLEAK" 2>/dev/null
}
trap cleanup EXIT

# Stop the automatic scan thread: only our manual scans should advance an
# object's consecutive-unreferenced run. An auto scan landing between two
# manual scans would change the result and make the test flaky.
echo scan=off > "$KMEMLEAK" 2>/dev/null

# Create a fresh, aged set of orphan objects from the helper module's init
# path (its kmalloc/vmalloc/percpu allocations are dropped right away).
# Pre-existing reported leaks are greyed first ("clear") so only our
# orphans are counted. The module is left loaded on purpose: once it is
# unloaded its symbols are gone, so the orphan backtraces no longer resolve
# to [kmemleak_test] and could not be matched below.
gen_orphans() {
	rmmod "$MODULE" 2>/dev/null
	echo clear > "$KMEMLEAK"
	modprobe "$MODULE" || skip "failed to load $MODULE"
	sleep "$AGE"
}

scan() { echo scan > "$KMEMLEAK"; }

# Number of helper-module orphans currently reported by kmemleak. Matching
# the module's own backtrace ([kmemleak_test]) keeps the count immune to
# unrelated leaks on the running system. kmemleak only lists an object here
# once it has been reported, so this reflects the confirmation gating.
count_orphans() {
	c=$(grep -c '\[kmemleak_test\]' "$KMEMLEAK" 2>/dev/null)
	echo "${c:-0}"
}

# 0) the parameter reads back what was written.
echo 3 > "$PARAM"
[ "$(cat "$PARAM")" = "3" ] || fail "min_unref_scans did not read back as 3"

# 1) min_unref_scans=1 (default): orphans reported on the first scan. This
#    also establishes that the helper produces detectable orphans here.
echo 1 > "$PARAM"
gen_orphans
scan
first=$(count_orphans)
[ "$first" -gt 0 ] ||
	skip "$MODULE produced no detectable orphans (cannot test min_unref_scans)"

# 2) min_unref_scans=2: nothing reported after the first scan, reported
#    after the second. The first-scan-zero check is the core regression.
echo 2 > "$PARAM"
gen_orphans
scan; s1=$(count_orphans)
scan; s2=$(count_orphans)
[ "$s1" -eq 0 ] || fail "min_unref_scans=2: $s1 orphan(s) reported after the 1st scan (must be 0)"
[ "$s2" -gt 0 ] || fail "min_unref_scans=2: no report on the 2nd scan (false negative)"

pass "min_unref_scans=1 immediate; =2 gated to 2nd scan (counts $first/$s1/$s2); param read-back ok"
