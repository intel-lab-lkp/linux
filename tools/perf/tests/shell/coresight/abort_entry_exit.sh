#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# CoreSight abort entry and exit (exclusive)

# shellcheck source=../lib/coresight_exception.sh
. "$(dirname "$0")/../lib/coresight_exception.sh"

test_abort_entry_exit()
{
	# A demand page fault retries the load at the same PC after resolution.
	if ! record_trace page_fault_loop; then
		cat "$tmpdir/record.log"
		echo "Failed to record the page fault workload"
		return 1
	fi
	decode_trace || return 1
	check_exception_pairs "int" "iret" 0 || return 1
}

setup_exception_test || exit $?
test_abort_entry_exit
