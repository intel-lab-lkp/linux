#!/bin/bash
# perf trace option tests
# SPDX-License-Identifier: GPL-2.0

set -e

. "$(dirname $0)"/lib/probe.sh

skip_if_no_perf_trace || exit 2

# The syscall tracepoints need root to open.
if [ "$(id -u)" != 0 ]; then
	echo "[Skip] No root permission"
	exit 2
fi

err=0
out=$(mktemp /tmp/__perf_test_trace_opts.out.XXXXX)

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

run_trace() {
	local desc="$1" expect="$2"

	shift 2
	echo "Trace ${desc}"
	if ! perf trace "$@" > "${out}" 2>&1; then
		echo "Trace ${desc} [Failed: perf trace failed]"
		cat "${out}"
		err=1
		return
	fi
	if [ -n "${expect}" ] && ! grep -qE "${expect}" "${out}"; then
		echo "Trace ${desc} [Failed: missing '${expect}']"
		cat "${out}"
		err=1
		return
	fi
	echo "Trace ${desc} [Success]"
}

# Page faults are software events, not syscalls, and exercise the
# separate page fault printing path.
run_trace "pagefaults" "fault \[" -F all -- perf test -w noploop 1
run_trace "minor pagefaults" "fault \[" -F min -- perf test -w noploop 1
run_trace "major pagefaults" "" -F maj -- sleep 0.1

# sched_stat_runtime feeds the "sched" summary counters.
run_trace "sched" "" --sched -s -- sleep 0.1

run_trace "syscalls" "" --syscalls -- true
run_trace "events" "openat" -e openat -e read -e write -- cat /dev/null
run_trace "failure" "" --failure -- false
run_trace "errno summary" "" --errno-summary -s -- cat /dev/null
run_trace "summary" "" -S --summary-mode=total -- cat /dev/null
run_trace "summary thread" "" -s --summary-mode=thread -- cat /dev/null
run_trace "duration filter" "" --duration 0.01 -- cat /dev/null
run_trace "max events" "" --max-events 5 -e write -- seq 1 100
run_trace "sort events" "" --sort-events -e write -- echo hello
run_trace "call graph" "" --call-graph fp --max-stack 5 --min-stack 1 \
	-e write -- echo hello
run_trace "kernel syscall graph" "" --kernel-syscall-graph \
	--call-graph fp -e write -- echo hello
run_trace "libtraceevent print" "" --libtraceevent_print \
	-e kmem:kmalloc -- true
run_trace "filter pids" "" --filter-pids 1 -- cat /dev/null
run_trace "proc map timeout" "" --proc-map-timeout 100 -- cat /dev/null
run_trace "mmap pages" "" -m 8 -- sleep 0.1
run_trace "delay" "" -D 10 -- sleep 0.1
run_trace "no inherit" "" --no-inherit -- sleep 0.1
run_trace "uid" "" -u "$(id -un)" -- true
run_trace "cpu" "" -C 0 -- sleep 0.1
run_trace "pid" "" -p $$ -- sleep 0.1
run_trace "tid" "" -t $$ -- sleep 0.1
run_trace "verbose" "" -v -e write -- echo hello
run_trace "bitmask list" "" --bitmask-list -e openat -- cat /dev/null
run_trace "print sample" "" --print-sample -e write -- echo hello

echo "Trace invalid pagefault type"
if perf trace -F bogus -- true > "${out}" 2>&1; then
	echo "Trace invalid pagefault type [Failed: accepted invalid value]"
	err=1
fi

cleanup
exit $err
