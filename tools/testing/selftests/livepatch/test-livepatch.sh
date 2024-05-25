#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_livepatch
MOD_REPLACE=test_klp_atomic_replace

setup_config


# - load a livepatch that modifies the output from /proc/cmdline and
#   verify correct behavior
# - unload the livepatch and make sure the patch was removed

start_test "basic function patching"

load_lp $MOD_LIVEPATCH

if [[ "$(cat /proc/cmdline)" != "$MOD_LIVEPATCH: this has been live patched" ]] ; then
	echo -e "FAIL\n\n"
	die "livepatch kselftest(s) failed"
fi

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

if [[ "$(cat /proc/cmdline)" == "$MOD_LIVEPATCH: this has been live patched" ]] ; then
	echo -e "FAIL\n\n"
	die "livepatch kselftest(s) failed"
fi

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"


# - load a livepatch that modifies the output from /proc/cmdline and
#   verify correct behavior
# - load another livepatch and verify that both livepatches are active
# - unload the second livepatch and verify that the first is still active
# - unload the first livepatch and verify none are active

start_test "multiple livepatches"

load_lp $MOD_LIVEPATCH

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

load_lp $MOD_REPLACE replace=0

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

disable_lp $MOD_REPLACE
unload_lp $MOD_REPLACE

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
$MOD_LIVEPATCH: this has been live patched
% insmod test_modules/$MOD_REPLACE.ko replace=0
livepatch: enabling patch '$MOD_REPLACE'
livepatch: '$MOD_REPLACE': initializing patching transition
livepatch: '$MOD_REPLACE': starting patching transition
livepatch: '$MOD_REPLACE': completing patching transition
livepatch: '$MOD_REPLACE': patching complete
$MOD_LIVEPATCH: this has been live patched
$MOD_REPLACE: this has been live patched
% echo 0 > /sys/kernel/livepatch/$MOD_REPLACE/enabled
livepatch: '$MOD_REPLACE': initializing unpatching transition
livepatch: '$MOD_REPLACE': starting unpatching transition
livepatch: '$MOD_REPLACE': completing unpatching transition
livepatch: '$MOD_REPLACE': unpatching complete
% rmmod $MOD_REPLACE
$MOD_LIVEPATCH: this has been live patched
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"


# - load a livepatch that modifies the output from /proc/cmdline and
#   verify correct behavior
# - load two addtional livepatches and check the number of livepatch modules
#   applied
# - load an atomic replace livepatch and check that the other three modules were
#   disabled
# - remove all livepatches besides the atomic replace one and verify that the
#   atomic replace livepatch is still active
# - remove the atomic replace livepatch and verify that none are active

start_test "atomic replace livepatch"

load_lp $MOD_LIVEPATCH

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

for mod in test_klp_syscall test_klp_callbacks_demo; do
	load_lp $mod
done

mods=(/sys/kernel/livepatch/*)
nmods=${#mods[@]}
if [ "$nmods" -ne 3 ]; then
	die "Expecting three modules listed, found $nmods"
fi

load_lp $MOD_REPLACE replace=1

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

mods=(/sys/kernel/livepatch/*)
nmods=${#mods[@]}
if [ "$nmods" -ne 1 ]; then
	die "Expecting only one moduled listed, found $nmods"
fi

# These modules were disabled by the atomic replace
for mod in test_klp_callbacks_demo test_klp_syscall $MOD_LIVEPATCH; do
	unload_lp "$mod"
done

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

disable_lp $MOD_REPLACE
unload_lp $MOD_REPLACE

grep 'live patched' /proc/cmdline > /dev/kmsg
grep 'live patched' /proc/meminfo > /dev/kmsg

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
$MOD_LIVEPATCH: this has been live patched
% insmod test_modules/test_klp_syscall.ko
livepatch: enabling patch 'test_klp_syscall'
livepatch: 'test_klp_syscall': initializing patching transition
livepatch: 'test_klp_syscall': starting patching transition
livepatch: 'test_klp_syscall': completing patching transition
livepatch: 'test_klp_syscall': patching complete
% insmod test_modules/test_klp_callbacks_demo.ko
livepatch: enabling patch 'test_klp_callbacks_demo'
livepatch: 'test_klp_callbacks_demo': initializing patching transition
test_klp_callbacks_demo: pre_patch_callback: vmlinux
livepatch: 'test_klp_callbacks_demo': starting patching transition
livepatch: 'test_klp_callbacks_demo': completing patching transition
test_klp_callbacks_demo: post_patch_callback: vmlinux
livepatch: 'test_klp_callbacks_demo': patching complete
% insmod test_modules/$MOD_REPLACE.ko replace=1
livepatch: enabling patch '$MOD_REPLACE'
livepatch: '$MOD_REPLACE': initializing patching transition
livepatch: '$MOD_REPLACE': starting patching transition
livepatch: '$MOD_REPLACE': completing patching transition
livepatch: '$MOD_REPLACE': patching complete
$MOD_REPLACE: this has been live patched
% rmmod test_klp_callbacks_demo
% rmmod test_klp_syscall
% rmmod $MOD_LIVEPATCH
$MOD_REPLACE: this has been live patched
% echo 0 > /sys/kernel/livepatch/$MOD_REPLACE/enabled
livepatch: '$MOD_REPLACE': initializing unpatching transition
livepatch: '$MOD_REPLACE': starting unpatching transition
livepatch: '$MOD_REPLACE': completing unpatching transition
livepatch: '$MOD_REPLACE': unpatching complete
% rmmod $MOD_REPLACE"


exit 0
