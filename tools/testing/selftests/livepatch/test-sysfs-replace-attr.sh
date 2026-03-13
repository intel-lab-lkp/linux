#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022 Song Liu <song@kernel.org>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_livepatch

setup_config

# - load a livepatch and verifies the sysfs replace attribute exists

start_test "check sysfs replace attribute"

load_lp $MOD_LIVEPATCH

check_sysfs_exists "$MOD_LIVEPATCH" "replace"
file_exists=$?

disable_lp $MOD_LIVEPATCH

unload_lp $MOD_LIVEPATCH

if [[ "$file_exists" == "0" ]]; then
	skip "sysfs attribute doesn't exists."
fi

start_test "sysfs test replace enabled"

MOD_LIVEPATCH=test_klp_atomic_replace
load_lp $MOD_LIVEPATCH replace=1

check_sysfs_rights "$MOD_LIVEPATCH" "replace" "-r--r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "replace" "1"

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko replace=1
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

start_test "sysfs test replace disabled"

load_lp $MOD_LIVEPATCH replace=0

check_sysfs_rights "$MOD_LIVEPATCH" "replace" "-r--r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "replace" "0"

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko replace=0
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

exit 0
