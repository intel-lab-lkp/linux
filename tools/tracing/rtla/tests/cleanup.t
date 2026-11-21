#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify that rtla removes the tracefs instance it created on the abnormal
# exit paths users actually hit:
#
#   - SIGINT during a run            (interactive Ctrl-C)
#   - SIGTERM during a run
#   - rtla's own -d/--duration timer
#   - a failing --on-threshold shell action
#   - several interrupted runs in a row, followed by a normal run
#
# A leaked instance pollutes the next invocation.
source tests/engine.sh
test_begin

set_timeout 2m

#
# These cases need to run rtla in the background and signal it, which
# engine.sh's check() (one foreground command) cannot express.
#
run_cleanup_cases() {
	local helper="$testdir/scripts/check-cleanup.sh"
	local out rc timeout_cmd skip

	ctr=$((ctr + 1))
	[ -z "$TEST_COUNT" ] && return

	if [ ! -x "$helper" ]; then
		echo "not ok $ctr - cleanup helper is not executable"
		return
	fi

	# check() normally does this before every test. Do the same here, both to
	# leave a clean tracer state and to make a hung helper terminate.
	reset_osnoise || {
		echo "ok $ctr - instances cleaned up on SIGINT, timeout and action failure # SKIP tracefs is not writable"
		return
	}
	timeout_cmd=${TIMEOUT:-timeout -v -k 15s 2m}

	out=$($timeout_cmd "$helper" "$RTLA" 2>&1)
	rc=$?
	if [ $rc -eq 0 ]; then
		# "SKIP:" is what the helper prints when it could not run at all
		# (no writable tracefs, or the binary is unusable); a per-case skip
		# says "note:" instead. So the first SKIP: line means nothing ran.
		skip=$(printf '%s\n' "$out" | sed -n 's/^SKIP: //p' | head -n 1)
		if [ -n "$skip" ]; then
			echo "ok $ctr - instances cleaned up on SIGINT, timeout and action failure # SKIP $skip"
			return
		fi
		printf '%s\n' "$out" | sed -n 's/^note: /# note: /p'
		echo "ok $ctr - instances cleaned up on SIGINT, timeout and action failure"
	elif [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		echo "not ok $ctr - instances cleaned up on SIGINT, timeout and action failure"
		echo "# helper timed out after 2m"
	else
		echo "not ok $ctr - instances cleaned up on SIGINT, timeout and action failure"
		echo "# helper exited $rc"
		printf '%s\n' "$out" | while read -r line; do
			echo "# $line"
		done
	fi
}

run_cleanup_cases

test_end
