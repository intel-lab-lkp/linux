#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# compaction-times python test

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh

# If we don't have the perf python module, we can't test
if ! "$PYTHON" -c 'import perf' > /dev/null 2>&1; then
	echo "Skipping test, perf python module not found"
	exit 2
fi

if ! perf check feature -q libtraceevent; then
	echo "Skipping test, perf built without libtraceevent"
	exit 2
fi

script_dir="$(dirname "$0")/../../python"
script_path="${script_dir}/compaction-times.py"

if [ ! -f "$script_path" ]; then
	echo "Skipping test, compaction-times.py not found at $script_path"
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
	echo "Testing compaction-times.py..."

	# Check for any compaction events to see if kernel supports it
	if ! perf list | grep -q "compaction:mm_compaction_begin"; then
		echo "Skipping test, compaction tracepoints not found"
		exit 2
	fi

	# Generate some events
	# We might not naturally trigger compaction in 0.5s sleep, but the script
	# should parse the empty or sparse file correctly without crashing.
	if ! perf record -e "compaction:*" -a -o "${temp_data}" -- sleep 0.5 >/dev/null 2>&1; then
		echo "Skipping test, perf record failed"
		exit 2
	fi

	# Run the script with some filters to validate filtering logic
	if ! "$PYTHON" "$script_path" -i "${temp_data}" >/dev/null; then
		echo "File mode default test failed."
		err=1
	fi

	if ! "$PYTHON" "$script_path" -i "${temp_data}" "0-0" >/dev/null; then
		echo "File mode strict PID filter test failed."
		err=1
	fi

	if ! "$PYTHON" "$script_path" -i "${temp_data}" "sleep" >/dev/null; then
		echo "File mode comm filter test failed."
		err=1
	fi

	if [ $err -eq 0 ]; then
		echo "File mode test passed."
	fi
}

test_file_mode

exit $err
