#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>
# Copyright (C) 2024 SUSE

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_speaker_livepatch
MOD_TARGET=test_klp_speaker

setup_config

# Use shadow variables, state, and callbacks to add "[APPLAUSE] "
# into the message printed by "welcome" parameter.

start_test "livepatch state callbacks"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH applause=1
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: applause_pre_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: applause_post_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': patching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: [APPLAUSE] Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
$MOD_LIVEPATCH: applause_pre_unpatch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: applause_post_unpatch_callback: state 10 (nope)
$MOD_LIVEPATCH: applause_shadow_dtor: freeing applause [] (nope)
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test failure of the "pre_patch" state callback.
#
# The livepatch should not get loaded. The test module should
# should stay unpatched which is checked by reading the "welcome"
# parameter.

start_test "failing pre_patch callback with -ENODEV"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_failing_mod $MOD_LIVEPATCH applause=1 pre_patch_ret=-19
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko applause=1 pre_patch_ret=-19
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: applause_pre_patch_callback: state 10
$MOD_LIVEPATCH: applause_pre_patch_callback: forcing err: -ENODEV
livepatch: failed to enable patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': canceling patching transition, going to unpatch
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
insmod: ERROR: could not insert module test_modules/$MOD_LIVEPATCH.ko: No such device
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test state callbacks handling with blocked and reverted transitons.
#
# The started patching transion never finishes. Only "pre_patch"
# callback is called.
#
# When reading the "welcome" parameter, the livepatched message
# is printed because it is a new process. But [APPLAUSE] is not
# printed because the "post_patch" callback has not been called.
#
# When the livepatch gets disabled, the current transiton gets
# reverted instead of starting a new disable transition. Only
# the "post_unpatch" callback is called.
start_test "blocked transition"

load_mod $MOD_TARGET block_doors=1
read_module_param $MOD_TARGET welcome

load_lp_nowait $MOD_LIVEPATCH applause=1
# Wait until the livepatch reports in-transition state, i.e. that it's
# stalled because of the process with the waiting speaker
loop_until 'grep -q '^1$' $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to stall transition"
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko block_doors=1
$MOD_TARGET: ${MOD_TARGET}_init
$MOD_TARGET: block_doors_func: Going to block doors.
$MOD_TARGET: do_block_doors: Started blocking doors.
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: applause_pre_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting patching transition
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: [] Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': reversing transition from patching to unpatching
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: applause_post_unpatch_callback: state 10 (nope)
$MOD_LIVEPATCH: applause_shadow_dtor: freeing applause [] (nope)
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

exit 0
