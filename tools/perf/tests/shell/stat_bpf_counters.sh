#!/bin/bash
# perf stat --bpf-counters test (exclusive)
# SPDX-License-Identifier: GPL-2.0

set -e

workload="perf test -w sqrtloop"
event=task-clock

# check whether $2 is within +/- 20% of $1
compare_number()
{
	first_num=$1
	second_num=$2

	# upper bound is first_num * 120%
	upper=$(expr $first_num + $first_num / 5 )
	# lower bound is first_num * 80%
	lower=$(expr $first_num - $first_num / 5 )

	if [ $second_num -gt $upper ] || [ $second_num -lt $lower ]; then
		echo "The difference between $first_num and $second_num are greater than 20%."
		exit 1
	fi
}

check_counts()
{
	base_count=$(echo "$1"|sed -e 's/[^0-9]*\([0-9]*\).*/\1/')
	bpf_count=$(echo "$2"|sed -e 's/[^0-9]*\([0-9]*\).*/\1/')

	if [ "$base_count" = "<not" ]; then
		echo "Skipping: $event event not counted"
		exit 2
	fi
	if [ "$bpf_count" = "<not" ]; then
		echo "Failed: $event not counted with --bpf-counters"
		exit 1
	fi
}

test_bpf_counters()
{
	printf "Testing --bpf-counters "
	base_count=$(perf stat --no-big-num -e "$event" -- $workload 2>&1 | awk "/$event/ {print \$1}")
	bpf_count=$(perf stat --no-big-num --bpf-counters -e "$event" -- $workload  2>&1 | awk "/$event/ {print \$1}")
	check_counts "$base_count" "$bpf_count"
	compare_number "$base_count" "$bpf_count"
	echo "[Success]"
}

test_bpf_modifier()
{
	printf "Testing bpf event modifier "
	stat_output=$(perf stat --no-big-num -e "$event/name=base_$event/,$event/name=bpf_$event/b" -- $workload 2>&1)
	base_count=$(echo "$stat_output"| awk "/base_$event/ {print \$1}")
	bpf_count=$(echo "$stat_output"| awk "/bpf_$event/ {print \$1}")
	check_counts "$base_count" "$bpf_count"
	compare_number "$base_count" "$bpf_count"
	echo "[Success]"
}

# skip if --bpf-counters is not supported
if ! perf stat -e $event --bpf-counters true > /dev/null 2>&1; then
	if [ "$1" = "-v" ]; then
		echo "Skipping: --bpf-counters not supported"
		perf --no-pager stat -e $event --bpf-counters true || true
	fi
	exit 2
fi

test_bpf_counters
test_bpf_modifier

exit 0
