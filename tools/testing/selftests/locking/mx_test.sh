#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

declare mx_min_log2_elems=10
declare mx_max_log2_elems=10
declare mx_inc_log2_elems=1
declare mx_seconds=1
declare mx_min_pad=8
declare mx_max_pad=8

# Kselftest framework requirement - SKIP code is 4.
declare ksft_skip=4

set_low_params() {
	mx_min_log2_elems=10
	mx_max_log2_elems=10
	mx_inc_log2_elems=1
	mx_seconds=1
	mx_min_pad=8
	mx_max_pad=8
}

set_med_params() {
	mx_min_log2_elems=8
	mx_max_log2_elems=12
	mx_inc_log2_elems=2
	mx_seconds=2
	mx_min_pad=8
	mx_max_pad=24
}

set_high_params() {
	mx_min_log2_elems=1
	mx_max_log2_elems=28
	mx_inc_log2_elems=4
	mx_seconds=5
	mx_min_pad=0
	mx_max_pad=56
}

usage() {
	printf "Usage: %s [-e low|medium|high] [-h]\n" "$(basename "$0")"
}

unload_module_exit_on_error() {
	local str="$1"
	local sts

	if [ -d /sys/module/mx_test ]; then
		/sbin/modprobe -q -r mx_test
		sts=$?
		if (( sts != 0 )); then
			echo "mx_test: $str Exit status: $sts"
			exit 1
		fi
	fi
}

run_test() {
	local test=$1
	local log2_elems
	local sts

	# Paranoia check. Unload if loaded.
	unload_module_exit_on_error "Failed to unload preloaded mx_test module."

	for log2_elems in $(seq $mx_min_log2_elems $mx_inc_log2_elems $mx_max_log2_elems); do
		/sbin/modprobe -q mx_test mx_test="$test" \
			       mx_nmbr_elems=$((1 << log2_elems)) \
			       mx_scnds_per_test="$mx_seconds" \
			       mx_min_padding="$mx_min_pad" \
			       mx_max_padding="$mx_max_pad"
		sts=$?

		if (( sts == 0 )); then
			unload_module_exit_on_error "Failed to unload module after test \"$test\"."
		else
			return $sts
		fi
	done

	return 0
}

find_eligible_tests() {
	local test

	# Note, the following modprobe will fail and module is not loaded afterwards
	/sbin/modprobe -q mx_test mx_test=non_existing_test

	# Skip "busted" and any "foo_busted"
	for test in $(dmesg -t | \
			      grep mx_test_types: | \
			      awk '{ print $2; }' | \
			      sort -u | \
			      grep -v busted); do
		echo $test
	done
}

# Run kernel selftests for lock-full and lock-less mutual exclusion tests.
# First, check module availability.
if ! /sbin/modprobe -q -n mx_test; then
	echo "mx_test: module mx_test is not found [SKIP]"
	exit $ksft_skip
fi

# Parse arguments
while getopts :e:h arg; do
	case $arg in
		e)
			case $OPTARG in
				low)
					set_low_params
					;;
				medium)
					set_med_params
					;;
				high)
					set_high_params
					;;
				*)
					echo "mx_test: Invalid effort level: $OPTARG"
					usage
					exit 1
					;;
			esac
			;;
		h)
			usage
			exit 0
			;;
		:)
			echo "mx_test: Option -$OPTARG requires an argument"
			usage
			exit 1
			;;
		\?)
			echo "mx_test: Unknown option -$OPTARG"
			usage
			exit 1
			;;
	esac
done

shift "$((OPTIND - 1))"

if (($# != 0)); then
	echo "mx_test: Unexpected argument: $1"
	usage
	exit 1
fi

# First, run "busted" to verify the test environment. If this test
# doesn't fail, there is no point in performing more tests in this
# environment.
run_test "busted"
sts=$?
if (( sts == 0 )); then
	echo mx_test: The test \"busted\" did not fail. Probable cause: UP kernel or too few CPUs online.
	echo mx_test: Hence, skipping the rest of the tests.
	exit $ksft_skip
fi

eligible_test="$(find_eligible_tests)"
if [ -z "$eligible_test" ]; then
	echo mx_test: No tests found. Skipping.
	exit $ksft_skip
fi

for test in $eligible_test; do
	run_test "$test"
	sts=$?
	if (( sts != 0 )); then
		echo mx_test: Test \"$test\" failed
		exit 1
	fi
done

echo mx_test: All tests passed
exit 0
