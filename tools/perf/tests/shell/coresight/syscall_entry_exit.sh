#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# CoreSight syscall entry and exit (exclusive)

# shellcheck source=../lib/coresight_exception.sh
. "$(dirname "$0")/../lib/coresight_exception.sh"

test_syscall_entry_exit()
{
	# callchain() invokes gettid() through SVC, which returns to PC + 4.
	if ! record_trace callchain; then
		cat "$tmpdir/record.log"
		echo "Failed to record the callchain workload"
		return 1
	fi
	decode_trace || return 1
	check_exception_pairs "syscall" "sysret" 4 || return 1
}

setup_exception_test || exit $?
test_syscall_entry_exit
