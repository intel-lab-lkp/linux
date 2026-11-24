#!/bin/bash
# perf timechart tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
perfdata=$(mktemp /tmp/__perf_timechart_test.perf.data.XXXXX)
output=$(mktemp /tmp/__perf_timechart_test.output.XXXXX.svg)

cleanup() {
	rm -f "${perfdata}"*
	rm -f "${output}"
	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_timechart() {
	NAME=$1
	OPTION=$2

	echo "perf timechart ${NAME} test"

	# Try to record timechart data.
	# perf timechart record uses system-wide recording and specific tracepoints.
	# If it fails (e.g. permissions, missing tracepoints), skip the test.
	if ! perf timechart record -o "${perfdata}" ${OPTION} true > /dev/null 2>&1; then
		echo "perf timechart ${NAME} test [Skipped: perf timechart record failed (permissions/events?)]"
		return
	fi

	# Generate the timechart
	if ! perf timechart -i "${perfdata}" -o "${output}" > /dev/null 2>&1; then
		echo "perf timechart ${NAME} test [Failed: perf timechart command failed]"
		err=1
		return
	fi

	# Check if output file exists and is not empty
	if [ ! -s "${output}" ]; then
		echo "perf timechart ${NAME} test [Failed: output file is empty or missing]"
		err=1
		return
	fi

	# Check if it looks like an SVG
	if ! grep -q "svg" "${output}"; then
		echo "perf timechart ${NAME} test [Failed: output doesn't look like SVG]"
		err=1
		return
	fi

	echo "perf timechart ${NAME} test [Success]"
}

test_timechart_report_option() {
	NAME=$1

	shift 1
	echo "perf timechart report ${NAME} test"

	if ! perf timechart -i "${perfdata}" -o "${output}" "$@" > /dev/null 2>&1; then
		echo "perf timechart report ${NAME} test [Failed: perf timechart command failed]"
		err=1
		return
	fi

	if [ ! -s "${output}" ]; then
		echo "perf timechart report ${NAME} test [Failed: output file is empty or missing]"
		err=1
		return
	fi

	if ! grep -q "svg" "${output}"; then
		echo "perf timechart report ${NAME} test [Failed: output doesn't look like SVG]"
		err=1
		return
	fi

	echo "perf timechart report ${NAME} test [Success]"
}

if ! perf check feature -q libtraceevent ; then
	echo "perf timechart is not supported. Skip."
        cleanup
	exit 2
fi

test_timechart "Basic" ""
test_timechart "IO-only" "-I"
test_timechart "Backtrace" "-g"

if [ ! -s "${perfdata}" ]; then
	echo "perf timechart report options [Skipped: no recorded data]"
	cleanup
	exit 0
fi

# Reuse the data recorded by the last test to exercise the report options.
test_timechart_report_option "width" -w 1024
test_timechart_report_option "proc-num" -n 4
test_timechart_report_option "topology" -t
test_timechart_report_option "highlight time" --highlight 1
test_timechart_report_option "highlight name" --highlight true
test_timechart_report_option "process" -p true
test_timechart_report_option "power-only" -P
test_timechart_report_option "tasks-only" -T
test_timechart_report_option "io options" --io-min-time 1us \
	--io-merge-dist 1us --io-skip-eagain
test_timechart_report_option "force" -f

cleanup
exit $err
