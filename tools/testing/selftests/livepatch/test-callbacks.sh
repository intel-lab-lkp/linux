#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_callbacks_demo
MOD_LIVEPATCH2=test_klp_callbacks_demo2
MOD_TARGET=test_klp_callbacks_mod
MOD_TARGET_BUSY=test_klp_callbacks_busy

setup_config

# Test loading multiple livepatches.  This test-case is mainly for comparing
# with the next test-case.
#
# - Load and unload two livepatches, pre and post (un)patch callbacks
#   execute as each patch progresses through its (un)patching
#   transition.

start_test "multiple livepatches"

load_lp $MOD_LIVEPATCH
load_lp $MOD_LIVEPATCH2
disable_lp $MOD_LIVEPATCH2
disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: pre_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: post_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_LIVEPATCH2.ko
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
$MOD_LIVEPATCH2: pre_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
$MOD_LIVEPATCH2: post_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': patching complete
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
$MOD_LIVEPATCH2: pre_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
$MOD_LIVEPATCH2: post_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
$MOD_LIVEPATCH: pre_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: post_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH2
% rmmod $MOD_LIVEPATCH"


# Load multiple livepatches, but the second as an 'atomic-replace'
# patch.  When the latter loads, the original livepatch should be
# disabled and *none* of its pre/post-unpatch callbacks executed.  On
# the other hand, when the atomic-replace livepatch is disabled, its
# pre/post-unpatch callbacks *should* be executed.
#
# - Load and unload two livepatches, the second of which has its
#   .replace flag set true.
#
# - Pre and post patch callbacks are executed for both livepatches.
#
# - Once the atomic replace module is loaded, only its pre and post
#   unpatch callbacks are executed.

start_test "atomic replace"

load_lp $MOD_LIVEPATCH
load_lp $MOD_LIVEPATCH2 replace=1
disable_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: pre_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: post_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_LIVEPATCH2.ko replace=1
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
$MOD_LIVEPATCH2: pre_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
$MOD_LIVEPATCH2: post_patch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': patching complete
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
$MOD_LIVEPATCH2: pre_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
$MOD_LIVEPATCH2: post_unpatch_callback: vmlinux
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% rmmod $MOD_LIVEPATCH2
% rmmod $MOD_LIVEPATCH"


exit 0
