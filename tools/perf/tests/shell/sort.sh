#!/bin/bash
# perf report and diff sort key tests
# SPDX-License-Identifier: GPL-2.0

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

testprog="perf test -w thloop"
testsym="test_loop"
sort_keys="pid,comm,dso,symbol,cpu,srcline,period,time,socket,weight,transaction,type,srcfile,cgroup"

skip_test_missing_symbol "${testsym}"

perfdata=$(mktemp /tmp/__perf_test_sort.perf.data.XXXXX)
out=$(mktemp /tmp/__perf_test_sort.out.XXXXX)
err=0

cleanup() {
	rm -f "${perfdata}" "${perfdata}".old
	rm -f "${out}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_report_sort() {
	echo "perf report sort keys test"
	perf record -o "${perfdata}" --sample-cpu ${testprog}
	if ! perf report -i "${perfdata}" --stdio -q -s "${sort_keys}" > "${out}" 2>&1; then
		echo "perf report sort keys test [Failed: perf report failed]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "${testsym}" "${out}"; then
		echo "perf report sort keys test [Failed missing ${testsym}]"
		err=1
		return
	fi
	echo "perf report sort keys test [Success]"
}

test_diff_sort() {
	echo "perf diff sort keys test"
	if ! perf diff -s "${sort_keys}" "${perfdata}" "${perfdata}" > "${out}" 2>&1; then
		echo "perf diff sort keys test [Failed: perf diff failed]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "${testsym}" "${out}"; then
		echo "perf diff sort keys test [Failed missing ${testsym}]"
		err=1
		return
	fi
	echo "perf diff sort keys test [Success]"
}

test_report_sort
test_diff_sort

cleanup
exit $err
