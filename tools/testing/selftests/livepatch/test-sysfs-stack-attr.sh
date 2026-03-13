#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022 Song Liu <song@kernel.org>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_livepatch
MOD_LIVEPATCH2=test_klp_callbacks_demo
MOD_LIVEPATCH3=test_klp_syscall

setup_config

# - load a livepatch and verifies the sysfs stack_order attribute exists

start_test "check sysfs stack_order attribute"

load_lp $MOD_LIVEPATCH

check_sysfs_exists "$MOD_LIVEPATCH" "stack_order"
file_exists=$?

disable_lp $MOD_LIVEPATCH

unload_lp $MOD_LIVEPATCH

if [[ "$file_exists" == "0" ]]; then
	skip "sysfs attribute doesn't exists."
fi

start_test "check sysfs stack_order permissions"

load_lp $MOD_LIVEPATCH

check_sysfs_rights "$MOD_LIVEPATCH" "stack_order" "-r--r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "stack_order" "1"

disable_lp $MOD_LIVEPATCH

unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

start_test "sysfs test stack_order value"

load_lp $MOD_LIVEPATCH

check_sysfs_value  "$MOD_LIVEPATCH" "stack_order" "1"

load_lp $MOD_LIVEPATCH2

check_sysfs_value  "$MOD_LIVEPATCH2" "stack_order" "2"

load_lp $MOD_LIVEPATCH3

check_sysfs_value  "$MOD_LIVEPATCH3" "stack_order" "3"

disable_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH2

check_sysfs_value  "$MOD_LIVEPATCH" "stack_order" "1"
check_sysfs_value  "$MOD_LIVEPATCH3" "stack_order" "2"

disable_lp $MOD_LIVEPATCH3
unload_lp $MOD_LIVEPATCH3

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_LIVEPATCH2.ko
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
$MOD_LIVEPATCH2: pre_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
$MOD_LIVEPATCH2: post_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': patching complete
% insmod test_modules/$MOD_LIVEPATCH3.ko
livepatch: enabling patch '$MOD_LIVEPATCH3'
livepatch: '$MOD_LIVEPATCH3': initializing patching transition
livepatch: '$MOD_LIVEPATCH3': starting patching transition
livepatch: '$MOD_LIVEPATCH3': completing patching transition
livepatch: '$MOD_LIVEPATCH3': patching complete
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
$MOD_LIVEPATCH2: pre_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
$MOD_LIVEPATCH2: post_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% rmmod $MOD_LIVEPATCH2
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH3/enabled
livepatch: '$MOD_LIVEPATCH3': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH3': starting unpatching transition
livepatch: '$MOD_LIVEPATCH3': completing unpatching transition
livepatch: '$MOD_LIVEPATCH3': unpatching complete
% rmmod $MOD_LIVEPATCH3
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

exit 0
