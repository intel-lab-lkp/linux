#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

UBLK_PROG="$(dirname "$0")/kublk"
expected="too many backing files (maximum is 4)"

if output=$("${UBLK_PROG}" add -t stripe a b c d e 2>&1); then
	echo "kublk accepted more than four backing files"
	exit 1
fi

if [ "${output}" != "${expected}" ]; then
	echo "unexpected error: ${output}"
	exit 1
fi
