#!/bin/bash -e
# CoreSight per-thread CPU attribution (exclusive)

# SPDX-License-Identifier: GPL-2.0

# If CoreSight is not available, skip the test
perf list pmu | grep -q cs_etm || exit 2

if ! tmpdir=$(mktemp -d /tmp/perf-cs-callchain-test.XXXXXX); then
	echo "mktemp failed"
	exit 1
fi

cleanup_files()
{
	rm -rf "$tmpdir"
	trap - EXIT TERM INT
}

trap cleanup_files EXIT
trap 'cleanup_files; exit 1' TERM INT

echo "Recording..."
perf record -o "$tmpdir/data" -e cs_etm//u --per-thread -- \
	taskset --cpu-list 0 taskset --cpu-list 1 taskset --cpu-list 2 true > /dev/null 2>&1

echo "Decoding..."
perf script -i "$tmpdir/data" --itrace=b -F comm,cpu 2> /dev/null | \
	grep -Eo '(taskset|true).*(\[[0-9]+\])' | \
	uniq | tail -n 3 > "$tmpdir/script" 2>/dev/null

# Check that the decode says it ran on CPU 0, 1, 2, in that order. TODO: The
# correct result should be "taskset [0,1,2], true 2" but we don't decode trace
# in order of the Perf events yet, so everything is associated with the last
# exec.
cat > "$tmpdir/expected" << EOF
true [000]
true [001]
true [002]
EOF

if ! diff -q "$tmpdir/script" "$tmpdir/expected"; then
	echo "FAIL: per-thread output doesn't match expected:"
	cat "$tmpdir/script"
	exit 1
fi

exit 0
