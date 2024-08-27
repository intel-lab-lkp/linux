#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 Lukas Hruska <lhruska@suse.cz>

. $(dirname $0)/functions.sh

MOD_LIVEPATCH=test_klp_extern
MOD_HELLO=test_klp_extern_hello
PARAM_HELLO=hello

setup_config

# - load a module to be livepatched
# - load a livepatch that modifies the output from 'hello' parameter
#   of the previously loaded module and verify correct behaviour
# - unload the livepatch and make sure the patch was removed
# - unload the module that was livepatched

start_test "livepatch with external symbol"

load_mod $MOD_HELLO

read_module_param $MOD_HELLO $PARAM_HELLO

load_lp $MOD_LIVEPATCH

read_module_param $MOD_HELLO $PARAM_HELLO

disable_lp $MOD_LIVEPATCH
unload_lp $MOD_LIVEPATCH

read_module_param $MOD_HELLO $PARAM_HELLO

unload_mod $MOD_HELLO

check_result "% insmod test_modules/$MOD_HELLO.ko
% echo \"$MOD_HELLO/parameters/$PARAM_HELLO: \$(cat /sys/module/$MOD_HELLO/parameters/$PARAM_HELLO)\"
$MOD_HELLO/parameters/$PARAM_HELLO: Hello from kernel module.
% insmod test_modules/$MOD_LIVEPATCH.ko
livepatch: enabling patch '$MOD_LIVEPATCH'
livepatch: '$MOD_LIVEPATCH': initializing patching transition
livepatch: '$MOD_LIVEPATCH': starting patching transition
livepatch: '$MOD_LIVEPATCH': completing patching transition
livepatch: '$MOD_LIVEPATCH': patching complete
% echo \"$MOD_HELLO/parameters/$PARAM_HELLO: \$(cat /sys/module/$MOD_HELLO/parameters/$PARAM_HELLO)\"
$MOD_HELLO/parameters/$PARAM_HELLO: Hello from livepatched module.
% echo 0 > /sys/kernel/livepatch/$MOD_LIVEPATCH/enabled
livepatch: '$MOD_LIVEPATCH': initializing unpatching transition
livepatch: '$MOD_LIVEPATCH': starting unpatching transition
livepatch: '$MOD_LIVEPATCH': completing unpatching transition
livepatch: '$MOD_LIVEPATCH': unpatching complete
% rmmod $MOD_LIVEPATCH
% echo \"$MOD_HELLO/parameters/$PARAM_HELLO: \$(cat /sys/module/$MOD_HELLO/parameters/$PARAM_HELLO)\"
$MOD_HELLO/parameters/$PARAM_HELLO: Hello from kernel module.
% rmmod $MOD_HELLO"

exit 0
