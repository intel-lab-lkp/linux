#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>
# Copyright (C) 2024 SUSE

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_speaker_livepatch
MOD_LIVEPATCH2=test_klp_speaker_livepatch2
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
$MOD_TARGET: block_doors_func: Going to block doors.
$MOD_TARGET: do_block_doors: Started blocking doors.
$MOD_TARGET: ${MOD_TARGET}_init
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

# Test state callbacks handling with blocked and later unblocked
# transiton.
#
# Load the test module with the blocked operation. Then load the livepatch
# and the transition should get stuck. Then unblock the operation
# so that the transition could finish. Finally, disable the livepatch
# and unload the modules as usual.
#
# Note that every process is transitioned separately. This is visible
# on the difference between the welcome message printed when reading
# the "welcome" parameter and the same message printed by the unblocked
# do_block_doors() function.

start_test "(un)blocked transition"

load_mod $MOD_TARGET block_doors=1
read_module_param $MOD_TARGET welcome

load_lp_nowait $MOD_LIVEPATCH applause=1
# Wait until the livepatch reports in-transition state, i.e. that it's
# stalled because of the process with the waiting speaker
loop_until 'grep -q '^1$' $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to stall transition"
read_module_param $MOD_TARGET welcome

# Unblock the doors (livepatch transtition)
write_module_param "$MOD_TARGET" block_doors 0
# Wait until the livepatch reports that the transition has finished
loop_until 'grep -q '^0$' $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to finish transition"
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko block_doors=1
$MOD_TARGET: block_doors_func: Going to block doors.
$MOD_TARGET: do_block_doors: Started blocking doors.
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: applause_pre_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting patching transition
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: [] Ladies and gentleman, ...
% echo 0 > $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/block_doors
$MOD_TARGET: do_block_doors: Stopped blocking doors.
$MOD_TARGET: speaker_welcome: Hello, World! <--- from blocked doors
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
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test state callbacks handling with blocked disable transition.
#
# Load the livepatch first. Then load the test module with the blocking
# operation and disable the livepatch. The transition should get stuck.
# Finally, get rid of the blocked function so that the transition could
# finish and the livepatch could get unloaded.
#
# Note that every process is transitioned separately. This is visible
# on the difference between the welcome message printed when reading
# the "welcome" parameter and the same message printed by the unblocked
# do_block_doors() function.
start_test "blocked disable transition"

load_lp $MOD_LIVEPATCH applause=1
load_mod $MOD_TARGET block_doors=1
read_module_param $MOD_TARGET welcome

disable_lp_nowait $MOD_LIVEPATCH
# Wait until the livepatch reports in-transition state, i.e. that it's
# stalled because of the process with the waiting speaker
loop_until 'grep -q '^1$' $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to stall transition"
read_module_param $MOD_TARGET welcome

# Unblock the doors (livepatch transtition)
write_module_param "$MOD_TARGET" block_doors 0
# Wait until the livepatch reports that the transition has finished
loop_until 'test ! -f $SYSFS_KLP_DIR/$MOD_LIVEPATCH/transition' ||
	die "failed to finish transition"
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: applause_pre_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: applause_post_patch_callback: state 10
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_TARGET.ko block_doors=1
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_LIVEPATCH: lp_block_doors_func: Going to block doors (fixed).
$MOD_TARGET: do_block_doors: Started blocking doors.
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: [APPLAUSE] Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
$MOD_LIVEPATCH: applause_pre_unpatch_callback: state 10
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% echo 0 > $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/block_doors
$MOD_TARGET: do_block_doors: Stopped blocking doors.
$MOD_LIVEPATCH: lp_speaker_welcome: [] Ladies and gentleman, ... <--- from blocked doors
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: applause_post_unpatch_callback: state 10 (nope)
$MOD_LIVEPATCH: applause_shadow_dtor: freeing applause [] (nope)
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test loading multiple livepatches in parallel.
#
# Both livepatches fix the speaker's welcome message. The first one
# also adds the base "[APPLAUSE]". The second one adds an extra "[APPLAUSE2]",
# aka from another level of the concert hall.
#
# The per-state callbacks are called when the state is introduced or
# or removed.
#
# The [APPLAUSE] and [APPLAUSE2] strings should appear in the speaker's
# welcome message when the respective livepatches are enabled.
start_test "multiple livepatches in parallel"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH applause=1
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH2 applause2=1
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH2
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
% insmod test_modules/$MOD_LIVEPATCH2.ko applause2=1
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
$MOD_LIVEPATCH2: applause_pre_patch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
$MOD_LIVEPATCH2: applause_post_patch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': patching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH2: lp_speaker_welcome: [APPLAUSE][APPLAUSE2] Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
$MOD_LIVEPATCH2: applause_pre_unpatch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
$MOD_LIVEPATCH2: applause_post_unpatch_callback: state 11 (nope)
$MOD_LIVEPATCH2: applause_shadow_dtor: freeing applause [2] (nope)
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% rmmod $MOD_LIVEPATCH2
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

# Test loading multiple livepatches using the atomic replace.
#
# Both livepatches fix the speaker's welcome message. The first one
# also adds the base "[APPLAUSE]". The second one also enables
# "[APPLAUSE2]", aka from another level of the concert hall.
#
# In compare with the previous selftest, the 2nd livepatch has
# to enable both "add_applause" and "add_applause2" module parameters.
# By other words, the second livepatch has to support both states.
# Otherwise, the base "[APPLAUSE]" would get disabled.
#
# The first livepatch is replaced. It does not need to be explicitly
# disabled.
#
# The per-state callbacks are called when the state is introduced or
# or removed.
#
# The [APPLAUSE] and [APPLAUSE2] strings should appear in the speaker's
# welcome message when the respective livepatches are enabled.
start_test "atomic replace"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH applause=1
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH2 replace=1 applause=1 applause2=1
unload_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH2
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
% insmod test_modules/$MOD_LIVEPATCH2.ko replace=1 applause=1 applause2=1
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
$MOD_LIVEPATCH2: applause_pre_patch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
$MOD_LIVEPATCH2: applause_post_patch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': patching complete
% rmmod $MOD_LIVEPATCH
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH2: lp_speaker_welcome: [APPLAUSE][APPLAUSE2] Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
$MOD_LIVEPATCH2: applause_pre_unpatch_callback: state 10
$MOD_LIVEPATCH2: applause_pre_unpatch_callback: state 11
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
$MOD_LIVEPATCH2: applause_post_unpatch_callback: state 10 (nope)
$MOD_LIVEPATCH2: applause_shadow_dtor: freeing applause [] (nope)
$MOD_LIVEPATCH2: applause_post_unpatch_callback: state 11 (nope)
$MOD_LIVEPATCH2: applause_shadow_dtor: freeing applause [2] (nope)
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% rmmod $MOD_LIVEPATCH2
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

exit 0
