#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_callbacks_demo
MOD_LIVEPATCH2=test_klp_callbacks_demo2
MOD_TARGET=test_klp_callbacks_mod
MOD_TARGET_BUSY=test_klp_callbacks_busy

setup_config

# A similar test as the previous one, but force the "busy" kernel module
# to block the livepatch transition.
#
# The livepatching core will refuse to patch a task that is currently
# executing a to-be-patched function -- the consistency model stalls the
# current patch transition until this safety-check is met.  Test a
# scenario where one of a livepatch's target klp_objects sits on such a
# function for a long time.  Meanwhile, load and unload other target
# kernel modules while the livepatch transition is in progress.
#
# - Load the "busy" kernel module, this time make its work function loop
#
# - Meanwhile, the livepatch is loaded.  Notice that the patch
#   transition does not complete as the targeted "busy" module is
#   sitting on a to-be-patched function.
#
# - Load a second target module (this one is an ordinary idle kernel
#   module).  Note that *no* post-patch callbacks will be executed while
#   the livepatch is still in transition.
#
# - Request an unload of the simple kernel module.  The patch is still
#   transitioning, so its pre-unpatch callbacks are skipped.
#
# - Finally the livepatch is disabled.  Since none of the patch's
#   klp_object's post-patch callbacks executed, the remaining
#   klp_object's pre-unpatch callbacks are skipped.

start_test "busy target module"

load_mod $MOD_TARGET_BUSY block_transition=Y
load_lp_nowait $MOD_LIVEPATCH

# Wait until the livepatch reports in-transition state, i.e. that it's
# stalled on $MOD_TARGET_BUSY::busymod_work_func()
loop_until 'grep -q '^1$' $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to stall transition"

load_mod $MOD_TARGET
unload_mod $MOD_TARGET
disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET_BUSY

check_result "% insmod test_modules/$MOD_TARGET_BUSY.ko block_transition=Y
$MOD_TARGET_BUSY: ${MOD_TARGET_BUSY}_init
$MOD_TARGET_BUSY: busymod_work_func enter
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: pre_patch_callback: vmlinux
$MOD_LIVEPATCH: pre_patch_callback: $MOD_TARGET_BUSY -> [MODULE_STATE_LIVE] Normal state
livepatch: '$MOD_LIVEPATCH': starting patching transition
% insmod test_modules/$MOD_TARGET.ko
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_LIVEPATCH: pre_patch_callback: $MOD_TARGET -> [MODULE_STATE_COMING] Full formed, running module_init
$MOD_TARGET: ${MOD_TARGET}_init
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
$MOD_LIVEPATCH: post_unpatch_callback: $MOD_TARGET -> [MODULE_STATE_GOING] Going away
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': reversing transition from patching to unpatching
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: post_unpatch_callback: vmlinux
$MOD_LIVEPATCH: post_unpatch_callback: $MOD_TARGET_BUSY -> [MODULE_STATE_LIVE] Normal state
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET_BUSY
$MOD_TARGET_BUSY: busymod_work_func exit
$MOD_TARGET_BUSY: ${MOD_TARGET_BUSY}_exit"


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
