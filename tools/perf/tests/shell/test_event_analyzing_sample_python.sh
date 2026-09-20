#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# event_analyzing_sample python test

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
script_path="${script_dir}/event_analyzing_sample.py"

if [ ! -f "$script_path" ]; then
	echo "Skipping test, event_analyzing_sample.py not found at $script_path"
	exit 2
fi

err=0
temp_data=""
temp_db=""

cleanup() {
	rm -f "${temp_data}" "${temp_db}"
}

trap 'cleanup' EXIT TERM INT

temp_data=$(mktemp /tmp/perf.data.XXXXXX)
temp_db=$(mktemp /tmp/perf.db.XXXXXX)

test_file_mode() {
	echo "Testing event_analyzing_sample.py..."

	# Generate some events
	if ! perf record -o "${temp_data}" -- perf test -w noploop >/dev/null 2>&1; then
		echo "Skipping test, perf record failed"
		exit 2
	fi

	# Run the script
	if ! "$PYTHON" "$script_path" -i "${temp_data}" -d "${temp_db}" >/dev/null; then
		echo "File mode test failed."
		err=1
	else
		echo "File mode test passed."
	fi
}

test_file_mode

exit $err
