#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# netdev_times python test

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh

if ! "$PYTHON" -c 'import perf' > /dev/null 2>&1; then
	echo "Skipping test, perf python module not found"
	exit 2
fi

script_dir="$(dirname "$0")/../../python"
script_path="${script_dir}/netdev-times.py"

if ! perf check feature -q libtraceevent > /dev/null 2>&1; then
	echo "Skipping test, libtraceevent is disabled"
	exit 2
fi


if [ ! -f "$script_path" ]; then
	echo "Skipping test, netdev-times.py not found at $script_path"
	exit 2
fi

err=0
temp_data=""
temp_out=""

cleanup() {
	rm -f "${temp_data}" "${temp_out}"
}
trap 'cleanup' EXIT TERM INT

temp_data=$(mktemp /tmp/perf.data.XXXXXX)
temp_out=$(mktemp /tmp/perf.out.XXXXXX)

echo "Testing netdev-times.py..."

# Create a perf.data file. Force dropping a packet if tracepoint is available!
if ! perf record -e skb:kfree_skb -a -o "${temp_data}" \
	-- ping -c 1 127.0.0.1 >/dev/null 2>&1; then
	perf record -e cycles -o "${temp_data}" \
		-- perf test -w noploop >/dev/null 2>&1 || \
		{ echo "Skipping test, perf record failed"; exit 2; }
fi

# Check that the script executes
if ! "$PYTHON" "$script_path" -i "${temp_data}" > "${temp_out}"; then
	echo "netdev-times.py test failed"
	err=1
else
	echo "netdev-times test passed."
fi
rm -f "${temp_out}"

exit $err
