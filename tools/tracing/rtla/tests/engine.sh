#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
test_begin() {
	# Count tests to allow the test harness to double-check if all were
	# included correctly.
	ctr=0
	# Set test directory to the directory of the script
	scriptfile=$(realpath "$0")
	testdir=$(dirname "$scriptfile")
	[ -z "$RTLA" ] && RTLA="./rtla"
	[ -n "$TEST_COUNT" ] && echo "1..$TEST_COUNT"
}

reset_osnoise() {
	# Reset osnoise options to default and remove any dangling instances created
	# by improperly exited rtla runs.
	pushd /sys/kernel/tracing >/dev/null || return 1

	# Remove dangling instances created by previous rtla run
	echo 0 > tracing_thresh
	cd instances
	for i in osnoise_top osnoise_hist timerlat_top timerlat_hist
	do
		[ ! -d "$i" ] && continue
		rmdir "$i"
	done

	# Reset options to default
	# Note: those are copied from the default values of osnoise_data
	# in kernel/trace/trace_osnoise.c
	cd ../osnoise
	echo all > cpus
	echo DEFAULTS > options
	echo 1000000 > period_us
	echo 0 > print_stack
	echo 1000000 > runtime_us
	echo 0 > stop_tracing_total_us
	echo 0 > stop_tracing_us
	echo 1000 > timerlat_period_us

	popd >/dev/null
}

leftover_instances() {
	# Count the trace instances rtla should have removed on exit.
	local d count=0
	for d in osnoise_top osnoise_hist timerlat_top timerlat_hist osnoise_trace
	do
		[ -d "/sys/kernel/tracing/instances/$d" ] && count=$((count + 1))
	done
	echo $count
}

check_with_action() {
	# Same as "check", but run ACTION while rtla is running, so that a test
	# can interrupt the tool mid-run.
	# Arguments: test_name rtla_args action exit_code [expected [unexpected]]
	# The output of ACTION is appended to rtla's output, and the number of
	# leftover trace instances is always reported as LEFTOVER_INSTANCES=n.
	# ACTION can use $RTLA_PID to signal the tool.
	local test_name=$1 tested_command=$2 action=$3
	local expected_exitcode=${4:-0}
	local expected_output=$5 unexpected_output=$6
	local result exitcode fail failbuf pid

	ctr=$(($ctr + 1))
	[ -z "$TEST_COUNT" ] && return

	[ "$NO_RESET_OSNOISE" == 1 ] || reset_osnoise

	tmpdir=$(mktemp -d)
	pushd $tmpdir >/dev/null

	# eval so that the test can use shell syntax in its arguments, and exec
	# so that $! is the tool itself and not an intermediate shell: tests
	# signal $RTLA_PID directly.  timeout forwards the signal to its child.
	# The arguments are re-parsed by the same eval as in check(), so quoting
	# behaves identically; only exec and the quoted $RTLA are added.
	( eval "exec stdbuf -oL $TIMEOUT \"$RTLA\" $tested_command" ) > rtla.txt 2>&1 &
	pid=$!
	# The ACTION runs as a separate process, so RTLA_PID has to be exported
	# or the helper scripts see it as unset.
	export RTLA_PID=$pid

	sleep "${ACTION_DELAY:-3}"
	eval "$action" >> rtla.txt 2>&1

	wait $pid
	exitcode=$?
	echo "LEFTOVER_INSTANCES=$(leftover_instances)" >> rtla.txt

	result=$(cat rtla.txt)
	failbuf=''
	fail=0

	if [ -n "$expected_output" ] && ! grep -qE "$expected_output" <<< "$result"
	then
		fail=1
		failbuf+=$(printf "# Output match failed: \"%s\"" "$expected_output")
		failbuf+=$'\n'
	fi

	if [ -n "$unexpected_output" ] && grep -qE "$unexpected_output" <<< "$result"
	then
		fail=1
		failbuf+=$(printf "# Output non-match failed: \"%s\"" "$unexpected_output")
		failbuf+=$'\n'
	fi

	if [ $exitcode -eq $expected_exitcode ] && [ $fail -eq 0 ]
	then
		echo "ok $ctr - $test_name"
	else
		echo "not ok $ctr - $test_name"
		echo -n "$failbuf"
		echo "$result" | col -b | while read line; do echo "# $line"; done
		printf "#\n# exit code %s\n" $exitcode
	fi

	popd >/dev/null
	rm -r $tmpdir
}

check() {
	test_name=$0
	tested_command=$1
	expected_exitcode=${3:-0}
	expected_output=$4
	unexpected_output=$5
	# Simple check: run rtla with given arguments and test exit code.
	# If TEST_COUNT is set, run the test. Otherwise, just count.
	ctr=$(($ctr + 1))
	if [ -n "$TEST_COUNT" ]
	then
		# Reset osnoise options before running test.
		[ "$NO_RESET_OSNOISE" == 1 ] || reset_osnoise

		# Create a temporary directory to contain rtla output
		tmpdir=$(mktemp -d)
		pushd $tmpdir >/dev/null

		# Run rtla; in case of failure, include its output as comment
		# in the test results.
		result=$(eval stdbuf -oL $TIMEOUT "$RTLA" $2 2>&1); exitcode=$?
		failbuf=''
		fail=0

		# Test if the results matches if requested
		if [ -n "$expected_output" ] && ! grep -qE "$expected_output" <<< "$result"
		then
			fail=1
			failbuf+=$(printf "# Output match failed: \"%s\"" "$expected_output")
			failbuf+=$'\n'
		fi

		if [ -n "$unexpected_output" ] && grep -qE "$unexpected_output" <<< "$result"
		then
			fail=1
			failbuf+=$(printf "# Output non-match failed: \"%s\"" "$unexpected_output")
			failbuf+=$'\n'
		fi

		if [ $exitcode -eq $expected_exitcode ] && [ $fail -eq 0 ]
		then
			echo "ok $ctr - $1"
		else
			# Add rtla output and exit code as comments in case of failure
			echo "not ok $ctr - $1"
			echo -n "$failbuf"
			echo "$result" | col -b | while read line; do echo "# $line"; done
			printf "#\n# exit code %s\n" $exitcode
		fi

		# Remove temporary directory
		popd >/dev/null
		rm -r $tmpdir
	fi
}

check_with_osnoise_options() {
	# Do the same as "check", but with pre-set tracefs options.
	# Resets osnoise first, then writes the given tracefs option=value
	# pairs before running the check with NO_RESET_OSNOISE=1.
	# Arguments: test_name command exit_code expected_output [path=value ...]
	# Each path is relative to /sys/kernel/tracing/
	local arg1=$1
	local arg2=$2
	local arg3=$3
	local arg4=$4
	local opt option value

	# Apply tracefs options (if not dry run)
	if [ -n "$TEST_COUNT" ]
	then
		[ "$NO_RESET_OSNOISE" == 1 ] || reset_osnoise
		shift 4
		for opt in "$@"
		do
			[ -z "$opt" ] && continue
			option="${opt%%=*}"
			value="${opt#*=}"
			# Try to apply the option, ignore errors: when pre-setting fails
			# (e.g. kernel does not know the option), the test itself will likely
			# also fail.
			# Throwing an error here would cause the test to be incorrectly
			# skipped.
			echo "$value" > "/sys/kernel/tracing/$option"
		done
	fi

	NO_RESET_OSNOISE=1 check "$arg1" "$arg2" "$arg3" "$arg4"
}

check_top_hist() {
	# Test one command with both "top" and "hist" tools, replacing "TOOL" in
	# command with either "top" or "hist" respectively, and prefixing the test
	# names with "top " and "hist ".
	check "top $1" "$(echo "$2" | sed 's/TOOL/top/g')" "${@:3}"
	check "hist $1" "$(echo "$2" | sed 's/TOOL/hist/g')" "${@:3}"
}

check_top_q_hist() {
	# Same as above, but pass "-q" to top so that strings printed in main
	# loop are on their own line for top too, not only for hist.
	check "top $1" "$(echo "$2" | sed 's/TOOL/top -q/g')" "${@:3}"
	check "hist $1" "$(echo "$2" | sed 's/TOOL/hist/g')" "${@:3}"
}

set_timeout() {
	TIMEOUT="timeout -v -k 15s $1"
}

unset_timeout() {
	unset TIMEOUT
}

set_no_reset_osnoise() {
	NO_RESET_OSNOISE=1
}

unset_no_reset_osnoise() {
	unset NO_RESET_OSNOISE
}

test_end() {
	# If running without TEST_COUNT, tests are not actually run, just
	# counted. In that case, re-run the test with the correct count.
	[ -z "$TEST_COUNT" ] && TEST_COUNT=$ctr exec bash $0 || true
}

# Avoid any environmental discrepancies
export LC_ALL=C
unset_timeout
