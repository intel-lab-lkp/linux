#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022 Song Liu <song@kernel.org>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_livepatch

setup_config

# - load a livepatch and verifies the sysfs entries work as expected

start_test "sysfs test"

load_lp $MOD_LIVEPATCH

check_sysfs_rights "$MOD_LIVEPATCH" "" "drwxr-xr-x"
check_sysfs_rights "$MOD_LIVEPATCH" "enabled" "-rw-r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "enabled" "1"
check_sysfs_rights "$MOD_LIVEPATCH" "force" "--w-------"
check_sysfs_rights "$MOD_LIVEPATCH" "replace" "-r--r--r--"
check_sysfs_rights "$MOD_LIVEPATCH" "transition" "-r--r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "transition" "0"
check_sysfs_rights "$MOD_LIVEPATCH" "vmlinux/patched" "-r--r--r--"
check_sysfs_value  "$MOD_LIVEPATCH" "vmlinux/patched" "1"

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

start_test "sysfs test object/patched"

MOD_LIVEPATCH=test_klp_speaker_livepatch
MOD_TARGET=test_klp_speaker
load_lp $MOD_LIVEPATCH

# check the "patch" file changes as target module loads/unloads
check_sysfs_value  "$MOD_LIVEPATCH" "$MOD_TARGET/patched" "0"
load_mod $MOD_TARGET
check_sysfs_value  "$MOD_LIVEPATCH" "$MOD_TARGET/patched" "1"
unload_mod $MOD_TARGET
check_sysfs_value  "$MOD_LIVEPATCH" "$MOD_TARGET/patched" "0"

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_TARGET.ko
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_TARGET: ${MOD_TARGET}_init
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

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
