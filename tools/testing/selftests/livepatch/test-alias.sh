#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Harry Hsu <x90613@gmail.com>

. $(dirname $0)/functions.sh

MOD_TARGET=test_klp_alias_target
MOD_LIVEPATCH=test_klp_alias_patch

setup_config


# $MOD_TARGET provides two symbols that share a single address.  A
# livepatch naming both of them would push two klp_funcs of the same
# patch onto one ops->func_stack, leaving the redirection ambiguous, so
# klp_init_object_loaded() has to reject the object.
#
# - load the target module and verify it produces the original output
# - verify that a livepatch naming both aliases fails to load
# - verify that the target module has been left unpatched

start_test "livepatch of two aliased symbols in one object"

load_mod $MOD_TARGET

if [[ "$(cat /proc/$MOD_TARGET)" != "$MOD_TARGET: original output" ]] ; then
	echo -e "FAIL\n\n"
	die "livepatch kselftest(s) failed"
fi

load_failing_mod $MOD_LIVEPATCH

if [[ "$(cat /proc/$MOD_TARGET)" != "$MOD_TARGET: original output" ]] ; then
	echo -e "FAIL\n\n"
	die "livepatch kselftest(s) failed"
fi

unload_mod $MOD_TARGET

check_result "% insmod test_modules/$MOD_TARGET.ko
$MOD_TARGET: ${MOD_TARGET}_init
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: 'test_klp_alias_show' and 'test_klp_alias_show_alias' resolve to the same address, aliased symbols are not supported
insmod: ERROR: could not insert module test_modules/$MOD_LIVEPATCH.ko: Invalid parameters
% rmmod $MOD_TARGET
$MOD_TARGET: ${MOD_TARGET}_exit"


# The same object is initialized from klp_module_coming() when the
# livepatch is loaded while the target module is still absent.  There
# the error has to be propagated to the module loader instead.
#
# - load the livepatch, it is accepted because the object is not loaded
# - verify that loading the target module is refused afterwards

start_test "aliased symbols in a module coming after the livepatch"

load_lp $MOD_LIVEPATCH
load_failing_mod $MOD_TARGET
disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

check_result "% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% insmod test_modules/$MOD_TARGET.ko
livepatch: 'test_klp_alias_show' and 'test_klp_alias_show_alias' resolve to the same address, aliased symbols are not supported
livepatch: failed to initialize patch '$MOD_LIVEPATCH' for module '$MOD_TARGET' (-22)
livepatch: patch '$MOD_LIVEPATCH' failed for module '$MOD_TARGET', refusing to load module '$MOD_TARGET'
insmod: ERROR: could not insert module test_modules/$MOD_TARGET.ko: Invalid parameters
% echo 0 > $SYSFS_KLP_DIR/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH"

exit 0
