#!/bin/bash
# perf probe variable and line listing tests
# SPDX-License-Identifier: GPL-2.0

set -e

. "$(dirname $0)"/lib/probe.sh

skip_if_no_perf_probe || exit 2

err=0
out=$(mktemp /tmp/__perf_test_probe_vars.out.XXXXX)

cleanup() {
	rm -f "${out}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

run_probe() {
	local desc="$1" expect="$2"

	shift 2
	echo "Probe ${desc}"
	if ! perf probe "$@" > "${out}" 2>&1; then
		echo "Probe ${desc} [Failed: perf probe failed]"
		cat "${out}"
		err=1
		return
	fi
	if [ -n "${expect}" ] && ! grep -q "${expect}" "${out}"; then
		echo "Probe ${desc} [Failed: missing '${expect}']"
		cat "${out}"
		err=1
		return
	fi
	echo "Probe ${desc} [Success]"
}

# The variable and line listings need debug info, skip when the kernel
# was built without it.
if ! perf probe -V schedule > "${out}" 2>&1; then
	echo "Probe variable listing [Skipped: no debug info for schedule]"
	cleanup
	exit 2
fi

run_probe "vars" "Available variables at schedule" -V schedule
run_probe "vars long option" "Available variables at schedule" --vars schedule
run_probe "vars offset" "Available variables at schedule" -V schedule+0
run_probe "line" "schedule" -L schedule
run_probe "line long option" "schedule" --line schedule

if [ "$(id -u)" = 0 ]; then
	run_probe "funcs" "schedule" --funcs
	run_probe "funcs filter" "schedule" --funcs "sched*"
else
	echo "Probe funcs [Skipped: needs root to read the kernel symbols]"
fi

echo "Probe missing add argument"
if perf probe -a > "${out}" 2>&1; then
	echo "Probe missing add argument [Failed: accepted no argument]"
	err=1
else
	if ! grep -q "Usage:" "${out}"; then
		echo "Probe missing add argument [Failed: missing usage]"
		err=1
	fi
fi

cleanup
exit $err
