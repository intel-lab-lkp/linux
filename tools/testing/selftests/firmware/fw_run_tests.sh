#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This runs all known tests across all known possible configurations we could
# emulate in one run.

set -e

DIR="$(dirname $(readlink -f "$0"))"
source "${DIR}"/../kselftest/ktap_helpers.sh

TEST_DIR=$(dirname $0)
source $TEST_DIR/fw_lib.sh

export HAS_FW_LOADER_USER_HELPER=""
export HAS_FW_LOADER_USER_HELPER_FALLBACK=""
export HAS_FW_LOADER_COMPRESS=""

run_tests()
{
	proc_set_force_sysfs_fallback $1
	proc_set_ignore_sysfs_fallback $2
	$TEST_DIR/fw_filesystem.sh

	proc_set_force_sysfs_fallback $1
	proc_set_ignore_sysfs_fallback $2
	$TEST_DIR/fw_fallback.sh

	proc_set_force_sysfs_fallback $1
	proc_set_ignore_sysfs_fallback $2
	$TEST_DIR/fw_upload.sh

	ktap_test_pass "Completed"
}

run_test_config_0001()
{
	ktap_print_msg "-----------------------------------------------------"
	ktap_print_msg "Running kernel configuration test 1 -- rare"
	ktap_print_msg "Emulates:"
	ktap_print_msg "CONFIG_FW_LOADER=y"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER=n"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=n"
	run_tests 0 1
}

run_test_config_0002()
{
	ktap_print_msg "-----------------------------------------------------"
	ktap_print_msg "Running kernel configuration test 2 -- distro"
	ktap_print_msg "Emulates:"
	ktap_print_msg "CONFIG_FW_LOADER=y"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER=y"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=n"
	proc_set_ignore_sysfs_fallback 0
	run_tests 0 0
}

run_test_config_0003()
{
	ktap_print_msg "-----------------------------------------------------"
	ktap_print_msg "Running kernel configuration test 3 -- android"
	ktap_print_msg "Emulates:"
	ktap_print_msg "CONFIG_FW_LOADER=y"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER=y"
	ktap_print_msg "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=y"
	run_tests 1 0
}

ktap_print_header

check_mods
check_setup

if [ -f $FW_FORCE_SYSFS_FALLBACK ]; then
	ktap_set_plan "4"

	run_test_config_0001
	run_test_config_0002
	run_test_config_0003
else
	ktap_set_plan "2"

	ktap_print_msg "Running basic kernel configuration, working with your config"
	run_tests
fi

ktap_print_msg "Running namespace test: "
$TEST_DIR/fw_namespace $DIR/trigger_request
if [ $? -eq 0 ]; then
    ktap_test_pass "fw_namespace completed successfully"
else
    ktap_test_fail "fw_namespace failed"
fi

ktap_finished
