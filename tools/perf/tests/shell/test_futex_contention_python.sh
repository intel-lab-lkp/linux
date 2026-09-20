#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# futex-contention python test

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh

# If we don't have the perf python module, we can't test
if ! "$PYTHON" -c 'import perf' > /dev/null 2>&1; then
	echo "Skipping test, perf python module not found"
	exit 2
fi

script_dir="$(dirname "$0")/../../python"
script_path="${script_dir}/futex-contention.py"

if ! perf check feature -q libtraceevent > /dev/null 2>&1; then
	echo "Skipping test, libtraceevent is disabled"
	exit 2
fi


if [ ! -f "$script_path" ]; then
	echo "Skipping test, futex-contention.py not found at $script_path"
	exit 2
fi

err=0
temp_data=""

cleanup() {
	rm -f "${temp_data}"
}

trap 'cleanup' EXIT TERM INT

temp_data=$(mktemp /tmp/perf.data.XXXXXX)

test_file_mode() {
	echo "Testing futex-contention.py..."
	# Some systems might not have syscalls:sys_enter_futex
	if ! perf list | grep -q syscalls:sys_enter_futex; then
		echo "Skipping test, syscalls:sys_enter_futex not found"
		exit 2
	fi

	# Generate some futex events
	if ! perf record -e syscalls:sys_enter_futex,syscalls:sys_exit_futex -a -o "${temp_data}" \
		-- sleep 0.5 2>/dev/null; then
		echo "Skipping (record failed)"
		exit 2
	fi

	if ! "$PYTHON" "$script_path" -i "${temp_data}" >/dev/null; then
		echo "File mode test failed."
		err=1
	else
		echo "File mode test passed."
	fi
}

test_file_mode

exit $err
