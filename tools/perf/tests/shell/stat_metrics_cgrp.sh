#!/bin/bash
# perf stat metrics --for-each-cgroup test
# SPDX-License-Identifier: GPL-2.0

set -e

test_cgroups=

log_verbose() {
	echo "$1"
}

# skip if system-wide is not supported
check_system_wide()
{
	log_verbose "Checking system-wide..."
	if ! perf stat -a -e instructions sleep 0.01 > /dev/null 2>&1
	then
		log_verbose "Skipping: system-wide monitoring not supported"
		exit 2
	fi
}

# find two cgroups to measure
find_cgroups()
{
	log_verbose "Finding cgroups..."
	# try usual systemd slices first
	if [ -d /sys/fs/cgroup/system.slice ] && [ -d /sys/fs/cgroup/user.slice ]
	then
		test_cgroups="system.slice,user.slice"
		log_verbose "Found cgroups: ${test_cgroups}"
		return
	fi

	# try root and self cgroups
	find_cgroups_self_cgrp=$(grep perf_event /proc/self/cgroup | cut -d: -f3)
	if [ -z "${find_cgroups_self_cgrp}" ]
	then
		# cgroup v2 doesn't specify perf_event
		find_cgroups_self_cgrp=$(grep ^0: /proc/self/cgroup | cut -d: -f3)
	fi

	if [ -z "${find_cgroups_self_cgrp}" ]
	then
		test_cgroups="/"
	else
		test_cgroups="/,${find_cgroups_self_cgrp}"
	fi
	log_verbose "Found cgroups: ${test_cgroups}"
}

# Check if metric is reported for each cgroup
# $1: extra options (e.g. --bpf-counters)
check_metric_reported()
{
	local opts="$1"
	local output

	log_verbose "Running check_metric_reported with opts '${opts}'..."
	# Run perf stat
	if ! output=$(perf stat -a ${opts} \
			--metrics=insn_per_cycle \
			--for-each-cgroup "${test_cgroups}" \
			-x, sleep 0.1 2>&1)
	then
		echo "FAIL: perf stat failed with exit code $?"
		echo "Output: ${output}"
		exit 1
	fi

	log_verbose "perf stat output:"
	log_verbose "${output}"

	# Split test_cgroups by comma
	IFS=',' read -r -a cgrps <<< "${test_cgroups}"

	for cgrp in "${cgrps[@]}"; do
		# Find lines for this cgroup and metric
		local cgrp_lines
		cgrp_lines=$(echo "${output}" | grep -F "${cgrp}" | grep "insn_per_cycle" || true)

		if [ -z "${cgrp_lines}" ]
		then
			echo "FAIL: No metric lines found for cgroup '${cgrp}'"
			exit 1
		fi

		# Check for nan or nested
		if echo "${cgrp_lines}" | grep -q -i -E ",nan,|,nested,"
		then
			echo "FAIL: Invalid metric value (nan/nested) for cgroup '${cgrp}'"
			echo "Line: ${cgrp_lines}"
			exit 1
		fi
		# Check for empty metric value
		if echo "${cgrp_lines}" | grep -q -E ",,[[:space:]]*[^,]*insn_per_cycle"
		then
			echo "FAIL: Empty metric value for cgroup '${cgrp}'"
			echo "Line: ${cgrp_lines}"
			exit 1
		fi
	done
	log_verbose "check_metric_reported passed for opts '${opts}'"
}

check_system_wide
find_cgroups

# Test 1: Without BPF counters
check_metric_reported ""

# Test 2: With BPF counters (if supported)
log_verbose "Checking BPF support..."
if perf stat -a --bpf-counters --for-each-cgroup / true > /dev/null 2>&1
then
	log_verbose "BPF supported, running Test 2..."
	check_metric_reported "--bpf-counters"
else
	log_verbose "BPF not supported, skipping Test 2"
fi

exit 0
