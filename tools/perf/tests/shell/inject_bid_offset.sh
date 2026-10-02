#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# perf inject --sample-buildids test

set -e
set -o pipefail

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

sym="noploop"

skip_test_missing_symbol ${sym}

temp_dir=$(mktemp -d /tmp/perf-test-bid-offset.XXXXXXXXXX)

prog="perf test -w noploop"
[ "$(uname -m)" = "s390x" ] && prog="$prog 3"
err=0

cleanup() {
	local exit_code=${1:-$?}
	trap - EXIT TERM INT
	if [ "${exit_code}" -ne 0 ] || [ "${err}" -ne 0 ]; then
		echo "Test failed! Preserving temp directory: ${temp_dir}"
		exit 1
	fi
	if [[ "${temp_dir}" =~ ^/tmp/perf-test-bid-offset\. ]]; then
		rm -rf "${temp_dir}"
	fi
	exit 0
}

trap_cleanup() {
	local exit_code=$?
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup ${exit_code}
}
trap trap_cleanup EXIT TERM INT

compare_script() {
	local orig=$1
	local bid=$2
	local msg=$3

	perf script -F -ip -i "${orig}" > "${temp_dir}/script_orig.txt"
	perf script -F -ip -i "${bid}" > "${temp_dir}/script_bid.txt"

	if ! diff -q "${temp_dir}/script_orig.txt" "${temp_dir}/script_bid.txt"; then
		echo "${msg} [Failed - perf script output differs]"
		err=1
	else
		echo "${msg} [Success]"
	fi
}

compare_report() {
	local orig=$1
	local bid=$2
	local msg=$3

	perf report -i "${orig}" --stdio | grep -v '^#' > "${temp_dir}/report_orig.txt"
	perf report -i "${bid}" --stdio | grep -v '^#' > "${temp_dir}/report_bid.txt"

	if ! diff -q "${temp_dir}/report_orig.txt" "${temp_dir}/report_bid.txt"; then
		echo "${msg} [Failed - perf report output differs]"
		err=1
	else
		echo "${msg} [Success]"
	fi
}

test_basic() {
	echo "Test basic build id offset injection"
	local data="${temp_dir}/perf.data.basic"
	local data2="${temp_dir}/perf.data2.basic"

	perf record -e task-clock:u -o "${data}" ${prog} >/dev/null 2>&1
	perf inject --sample-buildids -i "${data}" -o "${data2}" >/dev/null 2>&1

	compare_script "${data}" "${data2}" "Basic injection"
}

test_callchain() {
	echo "Test Callchain build id offset injection"
	local data="${temp_dir}/perf.data.callchain"
	local data2="${temp_dir}/perf.data2.callchain"

	perf record -g -e task-clock:u -o "${data}" ${prog} >/dev/null 2>&1
	perf inject --sample-buildids -i "${data}" -o "${data2}" >/dev/null 2>&1

	compare_script "${data}" "${data2}" "Callchain injection"
}

test_pipe() {
	echo "Test pipe mode build id offset injection"
	local data="${temp_dir}/perf.data.pipe"
	local data2="${temp_dir}/perf.data2.pipe"

	perf record -e task-clock:u -o - ${prog} 2>/dev/null | tee "${data}" | \
		perf inject --sample-buildids -o "${data2}" >/dev/null 2>&1

	compare_script "${data}" "${data2}" "Pipe mode injection"
}

test_report() {
	echo "Test perf report consistency"
	local data="${temp_dir}/perf.data.report"
	local data2="${temp_dir}/perf.data2.report"

	perf record -e task-clock:u -o "${data}" ${prog} >/dev/null 2>&1
	perf inject --sample-buildids -i "${data}" -o "${data2}" >/dev/null 2>&1

	compare_report "${data}" "${data2}" "Report consistency"
}

test_pipe_report() {
	echo "Test pipe mode perf report consistency"
	local data="${temp_dir}/perf.data.pipe_report"
	local data2="${temp_dir}/perf.data2.pipe_report"

	perf record -e task-clock:u -o - ${prog} 2>/dev/null | tee "${data}" | \
		perf inject --sample-buildids -o "${data2}" >/dev/null 2>&1

	compare_report "${data}" "${data2}" "Pipe Report consistency"
}

test_kernel() {
	echo "Test kernel build id offset injection"
	local data="${temp_dir}/perf.data.kernel"
	local data2="${temp_dir}/perf.data2.kernel"

	# Not all systems allow kernel profiling, so we check if it succeeds first
	if ! perf record -e task-clock:k -o "${data}" ${prog} >/dev/null 2>&1; then
		echo "Kernel injection [Skipped - permissions or support missing]"
		return
	fi
	perf inject --sample-buildids -i "${data}" -o "${data2}" >/dev/null 2>&1

	compare_script "${data}" "${data2}" "Kernel injection"
}

test_record_buildid_offset() {
	echo "Test perf record --buildid-offset consistency"
	local data="${temp_dir}/perf.data.record_orig"
	local data2="${temp_dir}/perf.data2.record_bid"

	perf record -e task-clock:u -o "${data}" ${prog} >/dev/null 2>&1
	# We must have the binaries in .build-id cache, which the first record does natively.
	# Now record with --buildid-offset.
	if ! perf record --buildid-offset -e task-clock:u -o "${data2}" \
	    ${prog} >/dev/null 2>&1; then
		echo "Record --buildid-offset consistency [Skipped - not supported by kernel]"
		return
	fi

	# Verify symbolization resolves the workload symbol
	if ! perf report -i "${data2}" --stdio | grep -q "${sym}"; then
		echo "Record --buildid-offset consistency [Failed - missing ${sym} in perf report]"
		err=1
	else
		echo "Record --buildid-offset consistency [Success]"
	fi
}

test_basic
test_callchain
test_pipe
test_report
test_pipe_report
test_kernel
test_record_buildid_offset

cleanup ${err}
