#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>
# Copyright (C) 2024 SUSE

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_speaker_livepatch
MOD_TARGET=test_klp_speaker
MOD_TARGET2=test_klp_speaker2

setup_config

# Test basic livepatch enable/disable functionality when livepatching
# modules.
#
# Loading the livepatch module without the target module being loaded.
#
# The transition should succeed. It is basically just a reference for
# for the following tests.

start_test "module not present"

load_lp $MOD_LIVEPATCH
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "0"
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

# Load the target module before the livepatch module. Unload them
# in the reverse order.
#
# The expected state is double-checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module enable/disable livepatch"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test the module coming hook in the module loader.
#
# Load the livepatch before the target module. Unload them in
# the same order.
#
# The livepatch hook in the module loader should print a message
# about applying the livepatch to the target module.
#
# The expected state is double-checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module coming hook"

load_lp $MOD_LIVEPATCH
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "0"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"

disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_TARGET.ko
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: Ladies and gentleman, ...
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test the module going hook in the module loader.
#
# The livepatch hook in the module loader should print a message
# about reverting the livepatch to the target module.
#
# The expected state is double-checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module going hook"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"

unload_mod $MOD_TARGET
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "0"

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: Ladies and gentleman, ...
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

# Test the module coming and going hooks in the module loader.
#
# Load the livepatch before the target module. Unload them in the reverse order.
#
# Both livepatch hooks in the module loader should print a message
# about applying resp. reverting the livepatch to the target module.
#
# The expected state is double-checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module coming and going hooks"

load_lp $MOD_LIVEPATCH
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "0"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"

unload_mod $MOD_TARGET
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "0"

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
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: Ladies and gentleman, ...
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

# Test loading multiple targeted kernel modules.
#
# Load the first target module before the livepatch and the second one later.
# Disable and unload them in the opposite order.
#
# The module loader hooks should print a message about applying/reverting
# the livepatch for the 2nd module when it is being loaded/unloaded.
#
# The expected state is double-checked by reading "welcome" parameter
# of both target modules. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "multiple target modules"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"
check_object_patched $MOD_LIVEPATCH $MOD_TARGET2 "0"

load_mod $MOD_TARGET2
read_module_param $MOD_TARGET2 welcome
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"
check_object_patched $MOD_LIVEPATCH $MOD_TARGET2 "1"

unload_mod $MOD_TARGET2
check_object_patched $MOD_LIVEPATCH $MOD_TARGET "1"
check_object_patched $MOD_LIVEPATCH $MOD_TARGET2 "0"

disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: lp_speaker_welcome: Ladies and gentleman, ...
% insmod test_modules/$MOD_TARGET2.ko
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET2'
$MOD_TARGET2: ${MOD_TARGET}_init
% cat $SYSFS_MODULE_DIR/$MOD_TARGET2/parameters/welcome
$MOD_LIVEPATCH: lp_speaker2_welcome(2): Ladies and gentleman, ...
% rmmod $MOD_TARGET2
$MOD_TARGET2: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET2'
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat $SYSFS_MODULE_DIR/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"
