#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify that rtla removes the tracefs instance it created on the abnormal
# exit paths users actually hit:
#
#   - SIGINT during a run            (interactive Ctrl-C)
#   - rtla's own -d/--duration timer
#   - a failing --on-threshold shell action
#   - several interrupted runs in a row, followed by a normal run
#
# A leaked instance pollutes the next invocation, so this is worth pinning
# down in CI. The SIGTERM case is reported as a TAP TODO comment because
# rtla does not install a SIGTERM handler yet.
source tests/engine.sh
test_begin

set_timeout 2m

#
# These cases need to run rtla in the background and signal it, which
# engine.sh's check() (one foreground command) cannot express.
#
run_cleanup_cases() {
	local helper="$testdir/scripts/check-cleanup.sh"
	local out rc

	ctr=$((ctr + 1))
	[ -z "$TEST_COUNT" ] && return

	if [ ! -x "$helper" ]; then
		echo "not ok $ctr - cleanup helper is not executable"
		return
	fi

	out=$("$helper" "$RTLA" 2>&1)
	rc=$?
	if [ $rc -eq 0 ]; then
		# Surface known issues as TAP comments so they stay visible
		# even when the case passes.
		printf '%s\n' "$out" | grep '^TODO: ' | while read -r line; do
			echo "# ${line#TODO: }"
		done
		echo "ok $ctr - instances cleaned up on SIGINT, timeout and action failure"
	else
		echo "not ok $ctr - instances cleaned up on SIGINT, timeout and action failure"
		printf '%s\n' "$out" | while read -r line; do
			echo "# $line"
		done
	fi
}

run_cleanup_cases

test_end
