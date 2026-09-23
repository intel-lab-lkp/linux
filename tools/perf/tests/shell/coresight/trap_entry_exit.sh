#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# CoreSight emulated instruction entry and exit (exclusive)

# shellcheck source=../lib/coresight_exception.sh
. "$(dirname "$0")/../lib/coresight_exception.sh"

test_trap_entry_exit()
{
	# traploop reads ID_AA64ISAR0_EL1 from EL0. The kernel emulates MRS and
	# advances the saved PC, so IRET must resume four bytes after the trap.
	if ! record_trace traploop 256; then
		cat "$tmpdir/record.log"
		echo "Failed to record the traploop workload"
		return 1
	fi
	decode_trace || return 1
	# Page faults also appear as "int", but retry the same PC. Select MRS
	# Xt, ID_AA64ISAR0_EL1 by its encoding; the low five bits select Xt.
	check_exception_pairs "int.*insn: [01][[:xdigit:]] 06 38 d5" "iret" 4 || return 1
}

setup_exception_test || exit $?
test_trap_entry_exit
