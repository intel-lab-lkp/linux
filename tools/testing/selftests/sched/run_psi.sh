#!/bin/bash
# SPDX-License-Identifier: GPL-2.0


# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

# Just one node check is enough to detect psi
if [ ! -e /proc/pressure/cpu ]; then
	echo "PSI not present..."
	exit $ksft_skip
fi

echo ""
./psi_test cpu
if [ $? -ne 0 ]; then
	echo "CPU - [FAIL]"
else
	echo "CPU - [PASS]"
fi

echo ""
./psi_test memory
if [ $? -ne 0 ]; then
	echo "MEMORY - [FAIL]"
else
	echo "MEMORY - [PASS]"
fi

echo ""
./psi_test io
if [ $? -ne 0 ]; then
	echo "IO - [FAIL]"
else
	echo "IO - [PASS]"
fi
