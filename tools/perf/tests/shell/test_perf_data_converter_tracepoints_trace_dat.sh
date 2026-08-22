#!/bin/bash
# 'perf data convert --to-trace-dat' command test
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright 2026, IBM Corporation
# Author: Tanushree Shah <tshah@linux.ibm.com>

set -e

err=0

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)

cleanup()
{
	rm -f "${perfdata}"
	rm -f "${result}"
	trap - exit term int
}

trap_cleanup()
{
	local exit_code=$?
	if [ $exit_code -ne 0 ] && [ $exit_code -ne 2 ]; then
		echo "Unexpected signal in ${FUNCNAME[1]}"
		cleanup
		exit 1
	fi
	cleanup
	exit $exit_code
}
trap trap_cleanup exit term int

result=$(mktemp /tmp/__perf_test.output.trace.dat.XXXXX)

# Check if libtraceevent support is available
if ! perf check feature libtraceevent
then
	echo "perf not linked with libtraceevent, skipping test"
	exit 2
fi

# Check if trace-cmd is available for validation
have_trace_cmd=0
if command -v trace-cmd >/dev/null 2>&1; then
	have_trace_cmd=1
fi

check_sched_switch()
{
	if ! perf list | grep -q 'sched:sched_switch'
	then
		echo "sched:sched_switch tracepoint not available, skipping test"
		exit 2
	fi
}

test_trace_converter_command()
{
	echo "Testing Perf Data Conversion Command to trace.dat"

	if ! perf record -e sched:sched_switch -a -o "$perfdata" -- sleep 0.1
	then
		echo "Failed to record perf data"
		err=1
		return
	fi

	if ! perf data convert --to-trace-dat "$result" --force -i "$perfdata"
	then
		echo "Perf Data Converter Command to trace.dat [FAILED]"
		err=1
		return
	fi

	if [ -f "$result" ] && [ -s "$result" ] ; then
		echo "Perf Data Converter Command to trace.dat [SUCCESS]"
	else
		echo "Perf Data Converter Command to trace.dat [FAILED]"
		err=1
	fi
}

test_trace_converter_pipe()
{
	echo "Testing Perf Data Conversion Command to trace.dat (Pipe mode)"

	rm -f "$result"

	if ! perf record -e sched:sched_switch -a -o - -- sleep 0.1 | \
	   perf data convert --to-trace-dat "$result" --force -i -
	then
		echo "Perf Data Converter Command to trace.dat (Pipe mode) [FAILED]"
		err=1
		return
	fi

	if [ -f "$result" ] && [ -s "$result" ]; then
		echo "Perf Data Converter Command to trace.dat (Pipe mode) [SUCCESS]"
	else
		echo "Perf Data Converter Command to trace.dat (Pipe mode) [FAILED]"
		err=1
	fi
}

test_trace_converter_mixed_events()
{
	echo "Testing Perf Data Conversion with tracepoint and non-tracepoint events"

	rm -f "$result"

	# Record both tracepoint and non-tracepoint events
	if ! perf record -e sched:sched_switch,cpu-clock -a -o "$perfdata" -- sleep 0.1
	then
		echo "Failed to record perf data (mixed events)"
		err=1
		return
	fi

	if ! perf data convert --to-trace-dat "$result" --force -i "$perfdata"
	then
		echo "Perf Data Converter with mixed events [FAILED]"
		err=1
		return
	fi

	if [ -f "$result" ] && [ -s "$result" ] ; then
		echo "Perf Data Converter with mixed events [SUCCESS]"
	else
		echo "Perf Data Converter with mixed events [FAILED]"
		err=1
	fi
}

validate_trace_format()
{
	echo "Validating Perf Data Converted trace.dat file"

	if [ ! -f "$result" ] ; then
		echo "File not found [FAILED]"
		err=1
		return
	fi

	# If trace-cmd is available, use it to validate the file
	if [ "$have_trace_cmd" -eq 1 ] ; then
		if trace-cmd report -i "$result" >/dev/null 2>&1 ; then
			echo "trace-cmd can read the file [SUCCESS]"
		else
			echo "trace-cmd cannot read the file [FAILED]"
			err=1
		fi
	else
		# Without trace-cmd, just check file exists and has content
		if [ -s "$result" ] ; then
			echo "Output file exists and has content [SUCCESS]"
		else
			echo "Output file is empty [FAILED]"
			err=1
		fi
	fi
}

check_sched_switch
test_trace_converter_command
validate_trace_format
test_trace_converter_pipe
validate_trace_format
test_trace_converter_mixed_events
validate_trace_format

cleanup
exit ${err}
