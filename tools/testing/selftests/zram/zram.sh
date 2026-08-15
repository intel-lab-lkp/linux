#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# KTAP output helpers (ktap_test_pass, ktap_test_fail, ...).
DIR="$(dirname "$(readlink -f "$0")")"
# shellcheck source=../kselftest/ktap_helpers.sh
source "${DIR}"/../kselftest/ktap_helpers.sh

ktap_print_header
ktap_set_plan 2

if [ "$(id -u)" -ne 0 ]; then
	ktap_test_skip "zram01.sh: must be run as root"
	ktap_test_skip "zram02.sh: must be run as root"
	ktap_finished
fi

# Run a sub-test, fold its output into "# " diagnostic lines and report its
# exit code as the KTAP result.
run_one()
{
	local script=$1

	"${DIR}/$script" 2>&1 | sed 's/^/# /'
	local rc=${PIPESTATUS[0]}

	if [ "$rc" -eq 0 ]; then
		ktap_test_pass "$script"
	elif [ "$rc" -eq "$KSFT_SKIP" ]; then
		ktap_test_skip "$script"
	else
		ktap_test_fail "$script"
	fi
}

run_one zram01.sh
run_one zram02.sh

ktap_finished
