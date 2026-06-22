#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

# Execute the test program which has its interpreter set to $ORIGIN/mock_interp
# Note that mock_interp must be in the same directory.
dir=$(dirname "$0")
out=$("$dir"/test_prog 2>&1)
exit_code=$?

if [ $exit_code -eq 42 ] && [ "$out" = "Hello from mock interpreter!" ]; then
	echo "origin_interp: PASS"
	exit 0
else
	echo "origin_interp: FAIL (exit_code=$exit_code, output='$out')"
	exit 1
fi
