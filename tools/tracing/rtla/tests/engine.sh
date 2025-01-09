#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
test_begin() {
	# Count tests to allow the test harness to double-check if all were
	# included correctly.
	ctr=0
	[ -z "$RTLA" ] && RTLA="./rtla"
	[ -n "$TEST_COUNT" ] && echo "1..$TEST_COUNT"
}

check() {
	# Simple check: run rtla with given arguments and test exit code.
	# If TEST_COUNT is set, run the test. Otherwise, just count.
	ctr=$(($ctr + 1))
	if [ -n "$TEST_COUNT" ]
	then
		# Run rtla; in case of failure, include its output as comment
		# in the test results.
		result=$("$RTLA" $2 2>&1) && echo "ok $ctr - $1" || echo "not ok $ctr - $1
$(echo "$result" | while read line; do echo "# $line"; done)
"
	fi
}

test_end() {
	# If running without TEST_COUNT, tests are not actually run, just
	# counted. In that case, re-run the test with the correct count.
	[ -z "$TEST_COUNT" ] && TEST_COUNT=$ctr exec bash $0 || true
}
