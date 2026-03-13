#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

V=${V:=0}  # invoke as V=1 $0  for global verbose
RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
BLUE="\033[0;34m"
MAGENTA="\033[0;35m"
CYAN="\033[0;36m"
NC="\033[0;0m"
error_msg=""

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit 0 # nothing to test here, no good reason to fail.
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG_SUBMOD=m" $KCONFIG_CONFIG ; LACK_TMOD_SUBMOD=$?
    if [ $V -eq 1 ]; then
	echo LACK_DD_BUILTIN: $LACK_DD_BUILTIN
	echo LACK_TMOD: $LACK_TMOD
	echo LACK_TMOD_SUBMOD: $LACK_TMOD_SUBMOD
    fi
else
    LACK_DD_BUILTIN=0
    LACK_TMOD=0
    LACK_TMOD_SUBMOD=0
fi

function vx () {
    echo $1 > /sys/module/dynamic_debug/parameters/verbose
}

function ddgrep () {
    grep $1 /proc/dynamic_debug/control
}

function doprints () {
    cat /sys/module/test_dynamic_debug/parameters/do_prints
}

function ddcmd () {
    exp_exit_code=0
    num_args=$#
    if [ "${@:$#}" = "pass" ]; then
	num_args=$#-1
    elif [ "${@:$#}" = "fail" ]; then
        num_args=$#-1
	exp_exit_code=1
    fi
    args=${@:1:$num_args}
    output=$((echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo $output | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ $3 -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code"
	[ $3 == 1 ] && echo "Error: '$error_msg'"
        exit
    fi
}

# $1 - pattern to match, pattern in $1 is enclosed by spaces for a match ""\s$1\s"
# $2 - number of times the pattern passed in $1 is expected to match
# $3 - optional can be set either to "-r" or "-v"
#       "-r" means relaxed matching in this case pattern provided in $1 is passed
#       as is without enclosing it with spaces
#       "-v" prints matching lines
# $4 - optional when $3 is set to "-r" then $4 can be used to pass "-v"
function check_match_ct {
    pattern="\s$1\s"
    exp_cnt=0

    [ "$3" == "-r" ] && pattern="$1"
    let cnt=$(ddgrep "$pattern" | wc -l)
    if [ $V -eq 1 ] || [ "$3" == "-v" ] || [ "$4" == "-v" ]; then
        echo -ne "${BLUE}" && ddgrep $pattern && echo -ne "${NC}"
    fi
    [ $# -gt 1 ] && exp_cnt=$2
    if [ $cnt -ne $exp_cnt ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO check failed expected $exp_cnt on $1, got $cnt"
        exit
    else
        echo ": $cnt matches on $1"
    fi
}

# $1 - trace instance name
# #2 - if > 0 then directory is expected to exist, if <= 0 then otherwise
# $3 - "-v" for verbose
function check_trace_instance_dir {
    if [ -e /sys/kernel/tracing/instances/$1 ]; then
        if [ "$3" == "-v" ] ; then
            echo "ls -l /sys/kernel/tracing/instances/$1: "
            ls -l /sys/kernel/tracing/instances/$1
        fi
	if [ $2 -le 0 ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error trace instance \
		    '/sys/kernel/tracing/instances/$1' does exist"
	    exit
	fi
    else
	if [ $2 -gt 0 ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error trace instance \
		    '/sys/kernel/tracing/instances/$1' does not exist"
	    exit
        fi
    fi
}

function tmark {
    echo $* > /sys/kernel/tracing/trace_marker
}

# $1 - trace instance name
# $2 - line number
# $3 - if > 0 then the instance is expected to be opened, otherwise
# the instance is expected to be closed
function check_trace_instance {
    output=$(tail -n9 /proc/dynamic_debug/control | grep ": Opened trace instances" \
	    | xargs -n1 | grep $1)
    if [ "$output" != $1 ] && [ $3 -gt 0 ]; then
        echo -e "${RED}: $BASH_SOURCE:$2 trace instance $1 is not opened"
        exit
    fi
    if [ "$output" == $1 ] && [ $3 -le 0 ]; then
        echo -e "${RED}: $BASH_SOURCE:$2 trace instance $1 is not closed"
        exit
    fi
}

function is_trace_instance_opened {
    check_trace_instance $1 $BASH_LINENO 1
}

function is_trace_instance_closed {
    check_trace_instance $1 $BASH_LINENO 0
}

# $1 - trace instance directory to delete
# $2 - if > 0 then directory is expected to be deleted successfully, if <= 0 then otherwise
function del_trace_instance_dir() {
    exp_exit_code=1
    [ $2 -gt 0 ] && exp_exit_code=0
    output=$((rmdir /sys/kernel/debug/tracing/instances/$1) 2>&1)
    exit_code=$?
    error_msg=$(echo $output | cut -d ":" -f 3 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function error_log_ref {
    # to show what I got
    : echo "# error-log-ref: $1"
    : echo cat \$2
}

function ifrmmod {
    lsmod | grep $1 2>&1>/dev/null && rmmod $1
}

# $1 - text to search for
function search_trace() {
    search_trace_name 0 1 $1
}

# $1 - trace instance name, 0 for global event trace
# $2 - line number counting from the bottom
# $3 - text to search for
function search_trace_name() {
	if [ "$1" = "0" ]; then
	    buf=$(cat /sys/kernel/debug/tracing/trace)
	    line=$(tail -$2 /sys/kernel/debug/tracing/trace | head -1 | sed -e 's/^[[:space:]]*//')
	else
	    buf=$(cat /sys/kernel/debug/tracing/instances/$1/trace)
	    line=$(tail -$2 /sys/kernel/debug/tracing/instances/$1/trace | head -1 | \
		   sed -e 's/^[[:space:]]*//')
	fi
	if [ $2 = 0 ]; then
	    # whole-buf check
	    output=$(echo $buf | grep "$3")
	else
	    output=$(echo $line | grep "$3")
	fi
	if [ "$output" = "" ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO search for '$3' failed \
		    in line '$line' or '$buf'"
	    exit
	fi
	if [ $V = 1 ]; then
	    echo -e "${MAGENTA}: search_trace_name in $1 found: \n$output \nin:${BLUE} $buf ${NC}"
        fi
}

# $1 - error message to check
function check_err_msg() {
    if [ "$error_msg" != "$1" ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error message '$error_msg' \
		does not match with '$1'"
        exit
    fi
}

function basic_tests {
    echo -e "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd =_ # zero everything
    check_match_ct =p 0

    # module params are builtin to handle boot args
    check_match_ct '\[params\]' 4 -r
    ddcmd module params +mpf
    check_match_ct =pmf 4

    # multi-cmd input, newline separated, with embedded comments
    cat <<"EOF" > /proc/dynamic_debug/control
      module params =_				# clear params
      module params +mf				# set flags
      module params func parse_args +sl		# other flags
EOF
    check_match_ct =mf 3
    check_match_ct =mfsl 1
    ddcmd =_
}

function comma_terminator_tests {
    echo -e "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    # try combos of spaces & commas
    check_match_ct '\[params\]' 4 -r
    ddcmd module,params,=_		# commas as spaces
    ddcmd module,params,+mpf		# turn on module's pr-debugs
    check_match_ct =pmf 4
    ddcmd ,module ,, ,  params, -p
    check_match_ct =mf 4
    ddcmd " , module ,,, ,  params, -m"	#
    check_match_ct =f 4
    ddcmd =_
}

function test_percent_splitting {
    echo -e "${GREEN}# TEST_PERCENT_SPLITTING - multi-command splitting on % ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
	echo "SKIP - test requires test-dynamic-debug.ko"
	return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+pf%class,D2_KMS,+pt%class,D2_ATOMIC,+pm
    check_match_ct =pf 1
    check_match_ct =pt 1
    check_match_ct =pm 1
    check_match_ct test_dynamic_debug 23 -r
    # add flags to those callsites
    ddcmd class,D2_CORE,+mf%class,D2_KMS,+lt%class,D2_ATOMIC,+ml
    check_match_ct =pmf 1
    check_match_ct =plt 1
    check_match_ct =pml 1
    check_match_ct test_dynamic_debug 23 -r
    ifrmmod test_dynamic_debug
}

tests_list=(
    basic_tests
    comma_terminator_tests
    test_percent_splitting
)

# Run tests

ifrmmod test_dynamic_debug_submod
ifrmmod test_dynamic_debug

for test in "${tests_list[@]}"
do
    $test
    echo ""
done
echo -en "${GREEN}# Done on: "
date
echo -en "${NC}"
