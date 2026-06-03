#!/bin/bash -e
# Coresight deterministic workload decode (exclusive)

# SPDX-License-Identifier: GPL-2.0

# If Coresight is not available, skip the test
perf list pmu | grep -q cs_etm || exit 2

tmpdir=$(mktemp -d /tmp/__perf_test.coresight_deterministic.XXXXX)

cleanup() {
	rm -rf "${tmpdir}"
	trap - EXIT TERM INT
}

trap_cleanup() {
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

cf="$tmpdir/ctl"
af="$tmpdir/ack"
mkfifo "$cf" "$af"

# Start disabled and use the control FIFO to only record the workload and not
# startup.
perf record -o "$tmpdir/data" -e cs_etm//u -D -1 --control fifo:"$cf","$af" -- \
	perf test --workload-ctl fifo:"$cf","$af" -w deterministic > /dev/null 2>&1

perf script -i "$tmpdir/data" --itrace=i1i -F ip,srcline | \
	grep "deterministic.c" | uniq > "$tmpdir/script" 2>/dev/null


# Remove open brace lines as they may not be hit depending on the compiler
sed -i \
  -e '/deterministic.c:8$/d' \
  -e '/deterministic.c:15$/d' \
  -e '/deterministic.c:23$/d' \
  "$tmpdir/script"

cat > "$tmpdir/expected" << EOF
  deterministic.c:24
  deterministic.c:25
  deterministic.c:26
  deterministic.c:28
  deterministic.c:9
  deterministic.c:10
  deterministic.c:11
  deterministic.c:12
  deterministic.c:30
  deterministic.c:31
  deterministic.c:32
  deterministic.c:34
  deterministic.c:16
  deterministic.c:17
  deterministic.c:18
  deterministic.c:19
  deterministic.c:36
  deterministic.c:37
EOF

if ! diff -q "$tmpdir/script" "$tmpdir/expected"; then
	echo "FAIL: line numbers don't match expected: "
	head -n 100 "$tmpdir/script"
	cleanup
	exit 1
fi

cleanup
exit 0
