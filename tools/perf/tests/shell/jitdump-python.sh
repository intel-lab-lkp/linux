#!/bin/bash
# python profiling with jitdump
# SPDX-License-Identifier: GPL-2.0

if ! command -v python > /dev/null; then
    echo "Skip: no python found"
    exit 2
fi

SHELLDIR=$(dirname $0)
PERF_DATA=$(mktemp /tmp/__perf_test.perf.data.XXXXXX)

echo "Run python with -Xperf_jit"
perf record -k 1 -g --call-graph dwarf -o "${PERF_DATA}" -- python -Xperf_jit ${SHELLDIR}/jitdump-test.py

_PID=$(perf report -i "${PERF_DATA}" -F pid -q -g none | cut -d: -f1 -s)
PID=$(echo -n $_PID)  # remove newlines

echo "Generate JIT-ed DSOs using perf inject"
perf inject -i "${PERF_DATA}" -j -o "${PERF_DATA}.jit"

echo "Add JIT-ed DSOs to the build-ID cache"
for F in /tmp/jitted-${PID}-*.so; do
  perf buildid-cache -a "${F}"
done

echo "Check the symbol containing the script name"
NUM=$(perf report -i "${PERF_DATA}.jit" -s sym | grep -c jitdump-test.py)

echo "Found ${NUM} matching lines"

echo "Remove JIT-ed DSOs from the build-ID cache"
for F in /tmp/jitted-${PID}-*.so; do
  perf buildid-cache -r "${F}"
done

rm -f ${PERF_DATA} ${PERF_DATA}.jit /tmp/jit-${PID}.dump /tmp/jitted-${PID}-*.so

if [ "${NUM}" -eq 0 ]; then
    exit 1
fi
