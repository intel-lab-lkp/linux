#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4

if [ "$(id -u)" -ne 0 ]; then
	echo "safesetid-gid-policy-test: must be run as root" >&2
	exit "$ksft_skip"
fi

exec ./safesetid-gid-policy-test
