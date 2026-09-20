#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# perf script lazy symbol loading skip status

set -e

shelldir=$(dirname "$0")
PERF_LAZY_LOAD_SYMBOLS_TEST_HELPERS=1
. "${shelldir}"/script_lazy_load_symbols.sh
unset PERF_LAZY_LOAD_SYMBOLS_TEST_HELPERS

err=0
mark_skip
if [ "${err}" -ne 2 ]; then
	echo "Lazy-load skip status [Failed expected 2, got ${err}]"
	exit 1
fi

err=1
mark_skip
if [ "${err}" -ne 1 ]; then
	echo "Lazy-load skip status [Failed skip overwrote failure: ${err}]"
	exit 1
fi

echo "Lazy-load skip status [Success]"
