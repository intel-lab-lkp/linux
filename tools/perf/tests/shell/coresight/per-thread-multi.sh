#!/bin/bash -e
# CoreSight per-thread multiple threads (exclusive)

# SPDX-License-Identifier: GPL-2.0

# If CoreSight is not available, skip the test
perf list pmu | grep -q cs_etm || exit 2

if ! tmpdir=$(mktemp -d /tmp/perf-cs-callchain-test.XXXXXX); then
	echo "mktemp failed"
	exit 1
fi

cleanup_files()
{
	if [[ $parent ]]; then
		kill -9 $parent
		wait $parent || true
	fi
	if [[ $perf ]]; then
		kill -9 $perf
		wait $perf || true
	fi
	rm -rf "$tmpdir"
	trap - EXIT TERM INT
}

trap cleanup_files EXIT
trap 'cleanup_files; exit 1' TERM INT

# Launch 2 threads to run indefinitely
nthreads=2
perf test -w named_threads $nthreads 0 &
parent=$!

# While parent still exists, wait for the 2 children to spawn
while kill -0 "$parent" 2>/dev/null; do
  threads=(/proc/"$parent"/task/*)

  if (( ${#threads[@]} >= $(($nthreads + 1)))); then
    break
  fi

  sleep 0.1
done

echo "Recording..."
perf record -o "$tmpdir/data" -e cs_etm//u --per-thread -Se -m,64K --pid $parent > /dev/null 2>&1 &
perf=$!

sleep 1

kill $parent
wait $parent || true
unset parent
wait $perf
unset perf

echo "Decoding..."
perf script -i "$tmpdir/data" > "$tmpdir/script" 2>/dev/null

# Exit early unless there is a dedicated sink per core which only TRBE
# guarantees. This is because shared sinks will report BUSY if two threads try
# to use them at the same time
if ! ls /sys/bus/coresight/devices/trbe* > /dev/null 2>&1; then
	echo "No TRBE sinks, skipping output validation"
	exit 0
fi

# Check all threads were traced and they have the correct thread name and symbol
for i in $(seq 1 $nthreads); do
	if ! grep -q "thread${i} .* named_threads_thread${i}" "$tmpdir/script"; then
		echo "Error: thread${i} missing" >&2
		exit 1
	fi
done

exit 0
