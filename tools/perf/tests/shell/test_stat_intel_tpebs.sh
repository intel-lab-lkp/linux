#!/bin/bash
# test Intel TPEBS counting mode
# SPDX-License-Identifier: GPL-2.0

set e

# Use this event for testing because it should exist in all platforms
e=cache-misses:R

# Without this cmd option, default value or zero is returned
echo "Testing without --enable-tpebs-recording"
result=$(perf stat -e "$e" true 2>&1)
[[ "$result" =~ $e ]] || exit 1

# In platforms that do not support TPEBS, it should execute without error.
echo "Testing with --enable-tpebs-recording"
result=$(perf stat -e "$e" --enable-tpebs-recording -a sleep 0.01 2>&1)
[[ "$result" =~ "perf record" && "$result" =~ $e ]] || exit 1
