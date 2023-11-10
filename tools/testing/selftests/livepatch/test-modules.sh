#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_speaker_livepatch
MOD_LIVEPATCH2=test_klp_speaker_livepatch2
MOD_TARGET=test_klp_speaker
MOD_TARGET2=test_klp_speaker2

setup_config

# Test basic livepatch enable/disable functionality when livepatching
# modules.
#
# Load the target module before the livepatch module. Unload them
# in the reverse order.
#
# The expected state is checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module enable/disable livepatch"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% modprobe $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: Ladies and gentleman, ...
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% cat /sys/module/$MOD_TARGET/parameters/welcome
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
# The expected state is checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module coming hook"

load_lp $MOD_LIVEPATCH
load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% modprobe $MOD_TARGET
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: Ladies and gentleman, ...
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"


# Test the module going hook in the module loader.
#
# The livepatch hook in the module loader should print a message
# about reverting the livepatch to the target module.
#
# The expected state is checked by reading "welcome" parameter
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

check_result "% modprobe $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: Ladies and gentleman, ...
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
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
# The expected state is checked by reading "welcome" parameter
# of the target module. The livepatched variant should be printed
# when both the target and livepatch modules are loaded.

start_test "module coming and going hooks"

load_lp $MOD_LIVEPATCH
load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET
disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% modprobe $MOD_TARGET
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET'
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: Ladies and gentleman, ...
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET'
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"


# Use shadow variables, state, and callbacks to add "[APPLAUSE] "
# into the message printed by "welcome" parameter.

start_test "livepatch state callbacks"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH add_applause=1
read_module_param $MOD_TARGET welcome

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% modprobe $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH add_applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: setup_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: enable_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': patching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: [APPLAUSE] Ladies and gentleman, ...
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
$MOD_LIVEPATCH: disable_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: release_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Use shadow variables, state, and callbacks to add "[APPLAUSE] "
# into the message printed by "welcome" parameter.
#
# BUT make the "setup" callback fail.
#
# The livepatch should not get loaded. The test module should
# should stay unpatched which is checked by reading the "welcome"
# parameter.

start_test "failing livepatch setup callback with -ENODEV"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_failing_mod $MOD_LIVEPATCH add_applause=1 setup_ret=-19
read_module_param $MOD_TARGET welcome

unload_mod $MOD_TARGET

check_result "% modprobe $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH add_applause=1 setup_ret=-19
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: setup_applause_callback: state 1
$MOD_LIVEPATCH: setup_applause_callback: forcing err: -ENODEV
livepatch: failed to enable patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': canceling patching transition, going to unpatch
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
modprobe: ERROR: could not insert '$MOD_LIVEPATCH': No such device
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test loading multiple targeted kernel modules.  This test-case is
# mainly for comparing with the next test-case.
#
# The livepatch gets loaded between two target modules. It adds
# a livepatch state, callbacks, and shadow variable which would
# add "[APPLAUSE] " into the message printed when reading
# the "welcome" parameters of the two target modules.
#
# All four state callbacks should get called. And the message
# "[APPLAUSE] Ladies and gentleman, ..." should be printed when
# reading the "welcome" parameter while the livepatch is enabled.

start_test "multiple target modules"

load_mod $MOD_TARGET
read_module_param $MOD_TARGET welcome

load_lp $MOD_LIVEPATCH add_applause=1
read_module_param $MOD_TARGET welcome

load_mod $MOD_TARGET2
read_module_param $MOD_TARGET2 welcome

unload_mod $MOD_TARGET2
disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% modprobe $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH add_applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: setup_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
$MOD_LIVEPATCH: enable_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': patching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: [APPLAUSE] Ladies and gentleman, ...
% modprobe $MOD_TARGET2
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET2'
$MOD_TARGET2: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET2/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome2: [APPLAUSE] Ladies and gentleman, ...
% rmmod $MOD_TARGET2
$MOD_TARGET2: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET2'
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
$MOD_LIVEPATCH: disable_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: release_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"

# Test loading multiple livepatches.  This test-case is mainly for comparing
# with the next test-case.
#
# The patching and unpatching transition should be done for both livepatches.

start_test "multiple livepatches in parallel"

load_lp $MOD_LIVEPATCH
load_lp $MOD_LIVEPATCH2 noreplace=1
disable_lp $MOD_LIVEPATCH2
disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH

check_result "% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% modprobe $MOD_LIVEPATCH2 noreplace=1
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
livepatch: '$MOD_LIVEPATCH2': patching complete
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH2
% rmmod $MOD_LIVEPATCH"


# Load multiple livepatches, but the second as an 'atomic-replace'
# patch.
#
# The 2nd livepatch will replace the 1st one. As a result, the 1s patch
# can be removed wihtout the unpatch transition.

start_test "atomic replace"

load_lp $MOD_LIVEPATCH
load_lp $MOD_LIVEPATCH2
disable_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH2
unload_lp $MOD_LIVEPATCH

check_result "% modprobe $MOD_LIVEPATCH
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% modprobe $MOD_LIVEPATCH2
livepatch: enabling patch '$MOD_LIVEPATCH2'
livepatch: '$MOD_LIVEPATCH2': initializing patching transition
livepatch: '$MOD_LIVEPATCH2': starting patching transition
livepatch: '$MOD_LIVEPATCH2': completing patching transition
livepatch: '$MOD_LIVEPATCH2': patching complete
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH2/enabled
livepatch: '$MOD_LIVEPATCH2': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH2': starting unpatching transition
livepatch: '$MOD_LIVEPATCH2': completing unpatching transition
livepatch: '$MOD_LIVEPATCH2': unpatching complete
% rmmod $MOD_LIVEPATCH2
% rmmod $MOD_LIVEPATCH"

exit 0

# FIXME:
#
#    The test below does not work and I do not know why.
#
#    It seems that the livepatched function call_speaker()
#    is not on the stack of the workqueue worker which
#    is processing the speaker work.
#
#    Even though, there is a worker which is waiting in
#    speaker_wait_and_welcome(). But the stack looks like:
#
#      [<0>] msleep+0x36/0x40
#      [<0>] speaker_wait_and_welcome+0x40/0x60 [test_klp_speaker]
#      [<0>] process_scheduled_works+0x2b4/0x530
#      [<0>] worker_thread+0x174/0x340
#      [<0>] kthread+0x100/0x130
#      [<0>] ret_from_fork+0x2d/0x50
#      [<0>] ret_from_fork_asm+0x1b/0x30
#
#    Note that I have got the same stack by adding show_stack()
#    directly into speaker_wait_and_welcome() function.
#
#    Also note that the speaker must be waiting when the livepatch
#    gets loaded. The speaker work  is queued when
#    the "waiting_speaker=1" parameter is proceed. And
#    waiting_welcome_set() waits until the worker started waiting.
#
#    And the function is supposed to be on the stack. It is called
#    via the speaker->call() callback. And it is marked as noinline.
#
#    Note that "call_speaker() did not appear on the stack even
#    when I tried to call it directly from speaker_func().
#
#    BTW: speaker_func() is not on the stack either. And it can't
#         be inlined because it is callback for the workqueue work.
#
######################################################################
#
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
# Note:
#
#   - The started patching transion never finishes. Only "setup"
#     callback is called.
#
#   - When reading the "welcome" parameter, the livepatched message
#     is printed because it is a new process. But [APPLAUSE] is not
#     printed because the "enable" callback has not been called.
#
#   - When the livepatch gets disabled, the current transiton gets
#     reverted instead of starting a new disable transition. Only
#     the "remove" callback is called.
start_test "busy target module"

load_mod $MOD_TARGET waiting_welcome=1
# Wait until the asynchronous speaker started waiting.
loop_until 'grep -q '^1$' /sys/module/$MOD_TARGET/parameters/started_waiting' ||
	die "failed to stall transition"
read_module_param $MOD_TARGET welcome

load_lp_nowait $MOD_LIVEPATCH add_applause=1
# Wait until the livepatch reports in-transition state, i.e. that it's
# stalled because of the process with the waiting speaker
loop_until 'grep -q '^1$' /sys/kernel/livepatch/$MOD_LIVEPATCH/transition' ||
	die "failed to stall transition"
read_module_param $MOD_TARGET welcome

load_mod $MOD_TARGET2
read_module_param $MOD_TARGET2 welcome

unload_mod $MOD_TARGET2
disable_lp $MOD_LIVEPATCH
read_module_param $MOD_TARGET welcome

unload_lp $MOD_LIVEPATCH
unload_mod $MOD_TARGET

check_result "% modprobe $MOD_TARGET waiting_welcome=1
$MOD_TARGET: call_speaker: Calling speaker.
$MOD_TARGET: speaker_wait_and_welcome: Speaker started waiting.
$MOD_TARGET: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% modprobe $MOD_LIVEPATCH add_applause=1
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
$MOD_LIVEPATCH: setup_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': starting patching transition
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome: [] Ladies and gentleman, ...
% modprobe $MOD_TARGET2
livepatch: applying patch '$MOD_LIVEPATCH' to loading module '$MOD_TARGET2'
$MOD_TARGET2: ${MOD_TARGET}_init
% cat /sys/module/$MOD_TARGET2/parameters/welcome
$MOD_LIVEPATCH: livepatch_speaker_welcome2: [] Ladies and gentleman, ...
% rmmod $MOD_TARGET2
$MOD_TARGET2: ${MOD_TARGET}_exit
livepatch: reverting patch '$MOD_LIVEPATCH' on unloading module '$MOD_TARGET2'
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': reversing transition from patching to unpatching
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
$MOD_LIVEPATCH: release_applause_callback: state 1
livepatch: '$MOD_LIVEPATCH': unpatching complete
% cat /sys/module/$MOD_TARGET/parameters/welcome
$MOD_TARGET: speaker_welcome: Hello, World!
% rmmod $MOD_LIVEPATCH
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit
$MOD_TARGET: speaker_welcome: Hello, World!"

