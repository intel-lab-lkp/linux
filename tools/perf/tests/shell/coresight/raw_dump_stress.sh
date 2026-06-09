#!/bin/bash -e
# CoreSight raw dump stress (exclusive)

# SPDX-License-Identifier: GPL-2.0

if [ "$(id -u)" != 0 ]; then
	# Requires root for larger buffer size
	echo "[Skip] No root permission"
	exit 2
fi

# If CoreSight is not available, skip the test
perf list pmu | grep -q cs_etm || exit 2

tmpdir=$(mktemp -d /tmp/__perf_test.coresight_raw_dump_stress.XXXXX)

cleanup() {
	rm -r "${tmpdir}"
	trap - EXIT TERM INT
}

trap_cleanup() {
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

# Use exit snapshot to record 2M of trace to make about 80MB of raw dump data.
echo "Recording..."
perf record -e cs_etm/timestamp=0/u -m,2M -Se -o "$tmpdir/data" -- \
	perf test -w brstack 20000 > /dev/null 2>&1

# Test raw dump runs to completion but don't decode because that's too slow for
# a test
echo "Dumping raw trace..."
perf report --dump-raw-trace -i "$tmpdir/data" 2>/dev/null > "$tmpdir/rawdump"

size=$(stat -c%s "$tmpdir/rawdump")
if [ $size -gt $((50 * 1024 * 1024)) ]; then
	echo "PASS: Raw dump file is larger than 50MB"
	cleanup
	exit 0
fi

echo "FAIL: Got less than 50MB (${size} bytes)"
cleanup
exit 1
