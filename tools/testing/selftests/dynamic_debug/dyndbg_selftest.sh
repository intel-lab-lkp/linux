#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

# Standard kselftest exit codes
ksft_pass=0
ksft_fail=1
ksft_skip=4

ESC=$'\033'
RED="${ESC}[0;31m"
GREEN="${ESC}[0;32m"
YELLOW="${ESC}[0;33m"
BLUE="${ESC}[0;34m"
MAGENTA="${ESC}[0;35m"
CYAN="${ESC}[0;36m"
NC="${ESC}[0;0m"
# Environment Controls:
#   V=0,1,2 : Verbosity (0=concise summary, 1=verified assertions, 2=full captured outputs)
#   K=0     : Strict mode (fails with exit 1 on checksum drift or stale records)
#   K=1     : Soft-pass mode (prints DRIFT/STALE diffs, exits 0 with 'fake success')
#   K=2     : Silent soft-pass mode (suppresses DRIFT/STALE diffs, exits 0 with 'fake success')
V=${V:=0}
K=${K:=0}

# Sanitize V and K to ensure they are valid integers
if [[ ! "$V" =~ ^[0-9]+$ ]]; then
    V=0
fi
if [[ ! "$K" =~ ^[0-9]+$ ]]; then
    K=0
fi

function v_echo {
    [ "${V:-0}" -ge 1 ] && echo -e "$@"
}

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit $ksft_skip # nothing to test here, no good reason to fail.
}

lsmod >/dev/null 2>&1 || {
    echo -e "${RED}: lsmod requires /proc/modules ${NC}"
    # exit $ksft_skip # maybe later we can do more
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    v_echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
else
    # if no config, try runtime probes
    modprobe -n test_dynamic_debug 2>/dev/null ; LACK_TMOD=$?
    # assume builtin dyndbg if control exists (checked above)
    LACK_DD_BUILTIN=0
fi

function ifrmmod {
    [ "${LACK_TMOD:-0}" -eq 1 ] && return
    grep -q "^$1 " /proc/modules 2>/dev/null && rmmod $1
}

# Clean up any leftover loaded test modules at initialization
ifrmmod test_dynamic_debug_submod
ifrmmod test_dynamic_debug

# ===========================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ===========================================================================
DDCMD_LOG=""	# accumulate

function log_ddcmd {
    local cmd="$1"
    if [ "${IN_BOOKEND:-0}" -eq 1 ] && [ -n "$DDCMD_LOG" ]; then
        DDCMD_LOG="${DDCMD_LOG}; $cmd"
    else
        DDCMD_LOG="$cmd"
    fi
}

function my_modprobe {
    log_ddcmd "modprobe $*"
    modprobe "$@"
}

function set_param {
    local val="$1"
    local path="$2"
    log_ddcmd "echo $val > $path"
    echo "$val" > "$path"
}

function ddcmd () {
    # ddcmd <query_args> [range_pattern] [pass|fail|log]
    local args="$1"
    local range="$2"
    local action="${3:-pass}"
    local exp_exit=0

    [ "$action" = "fail" ] && exp_exit=1
    log_ddcmd "$args"

    # Update cumulative state-machine lineage
    if [[ "$args" == *"=_"* ]]; then
        CUMULATIVE_DDCMDS="$args"
    else
        CUMULATIVE_DDCMDS="${CUMULATIVE_DDCMDS}; $args"
    fi

    [ "$action" != "pass" ] && log_start
    [ -n "$range" ] && capture_before "$range"

    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1 )
    handle_exit_code $BASH_LINENO $FUNCNAME $? $exp_exit

    [ "$action" != "pass" ] && log_stop
    [ -n "$range" ] && verify_after_change
}

function ddcmd_err () {
    # ddcmd_err <query_args>
    # Semantic wrapper for parser syntax & error validation
    ddcmd "$1" "" fail
}

function ddcmd_load () {
    # ddcmd_load <query_args> <range_pattern> <workload_param_path> <workload_val>
    # Semantic wrapper for end-to-end filter setup and live workload logging
    local query="$1"
    local range="$2"
    local param_path="$3"
    local val="$4"

    # 1. Setup the control filters (using positional ddcmd range-check)
    ddcmd "$query" "$range"

    # 2. Execute the workload and capture syslog prints
    log_start
    echo "$val" > "$param_path"
    log_stop
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ "$3" -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() " \
            "expected to exit with code $exp_exit_code, got $3${NC}"
	[ "$3" == 1 ] && echo "Error: '$output'"
        exit $ksft_fail
    fi
}

# ==============================================================================
# TESTING STRATEGY 2.
#   do 1 to setup test expectations.
#   run logging-workload
#   capture output
#   hash-validate it against GOLDEN_SAMPLE db (at file end)
#
# ==============================================================================
# Source hash-based validation and state verification helper library
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR/syslog_hash_validation.sh"

# Define target validation file path
CONTROL_FILE="/proc/dynamic_debug/control"

# App-specific wrappers mapping to generic library helpers
function verify_control_slice {
    # $1 - pattern to slice
    # $2 - optional extra args
    verify_file_slice "$1" $CONTROL_FILE "$2"
}

function slice_and_hash_ddctrl {
    local slice=$(slice_by_grep "$1" "$CONTROL_FILE" | strip_control_linenos)
    echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

# ==============================================================================

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_prints)
    # $2 - parameter value (e.g. 1)
    local param="$1"
    local val="$2"

    # Make sure both modules are completely unloaded to trigger a fresh load
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # Capture and verify the load-time (modprobe) dmesg logs
    log_start
    my_modprobe test_dynamic_debug "${param}=${val}"
    my_modprobe test_dynamic_debug_submod

    # If it is a state-controlling parameter, trigger the
    # print-workload 'do_prints=1' inside the same syslog dmesg
    # capture bookends to verify their actual pr_debug logging!

    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        set_param 1 /sys/module/test_dynamic_debug/parameters/do_classes
    fi

    log_stop

    # Verify param write by direct readback
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        local readback=$(cat "/sys/module/test_dynamic_debug/parameters/${param}")
        if (( readback != val )); then
            echo -e "${RED}: param readback failed: ${param} ${val} != ${readback}${NC}"
            exit $ksft_fail
        else
            [ "$V" -ge 1 ] && \
		echo -e "${GREEN}✔ Parameter Readback Verified: ${param}=${readback}${NC}"
        fi
    fi

    # verify runtime unsetting
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then

        set_param 0 "/sys/module/test_dynamic_debug/parameters/${param}"
	verify_control_slice '\[test_dynamic_debug\]'

    fi
}

# ==============================================================================
# FEATURE TESTS (FT_*)
#
# test legal queries which should execute and return 0 (success)
# so we dont look for errors in dmesg
function FT_grammar_ok {
    v_echo "${GREEN}# GRAMMAR_OK_TESTS ${NC}"
    ddcmd "+_"
    ddcmd "-_"

    # use 4 keywords (max 9 words inc flags)
    ddcmd "module foo file bar.c func buz class D2_CORE +_"	# 4 keywords
    ddcmd "module foo file bar.c func buz class D2 line 100 +_" # 5 keywords

    # 3. Dedicated lineno range grammar assertions (side-effect-free proofs)
    ddcmd "line 42 +_"		# test exact line syntax
    ddcmd "line 10- +_"		# test open-ended line range (starting at 10)
    ddcmd "line -100 +_"	# test open-ended line range (ending at 100)
    ddcmd "line 10-100 +_"	# test closed-interval line range

    # 4. Dedicated colon-delimited file:line and file:func assertions
    ddcmd "file a_file.c:1-100 +_"	# test file:linerange syntax
    ddcmd "file b_file.c:30 +_"		# test file:exact_line syntax
    ddcmd "file c_file.c:c_func +_"	# test file:function_name syntax
    ddcmd "file c_file.c:start_* +_"	# test file:wildcard_function syntax

    # 5. Advanced formatting and separator checks (side-effect-free proofs)
    ddcmd "format \"space\\040here\" +_"	# test format query with octal escape
    #ddcmd "module,foo +_"		# test comma token separator syntax
    ddcmd "func *my_func* +_"		# test wildcard func syntax
    ddcmd "file drivers/usb/* +_"	# test wildcard file path syntax
}

# test grammar, no actual sites chosen/changed
# use dyndbg's embedded comments in queries
function FT_grammar_errs {
    v_echo "${GREEN}# GRAMMAR_ERROR_TESTS ${NC}"
    ddcmd =_
    local verbose

    # Reset before loop
    echo 0 > /sys/module/dynamic_debug/parameters/verbose

    # Sequence verbose level 0..3 to verify error diagnostics across all verbosity states!
    for verbose in 1 2 3; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	ddcmd_err 'module foo format "parse +p #! unclosed quote: parse +p'

	# comments in queries tell the error in the logs
	ddcmd_err "module foo unknown_keyword value	#! bad flag-op v, at start of value"
	ddcmd_err "module foo %pm	#! bad flag-op %, at start of %pm"
	ddcmd_err "module foo +pfmHKDD	#! unknown flag 'H'"

	ddcmd_err "w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w 14 w15 w16 #! too many words, legal max <=15"
	ddcmd_err "func w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 +p #! unknown keyword \"w3\""
	ddcmd_err "module foo line =_ #! expecting pairs of match-spec <value>"

	# match-spec duplicate keywords
	ddcmd_err "func foo func bar =_	#! match-spec:func val:foo overridden by bar"
	ddcmd_err "file foo.c file bar.c =_	#! match-spec:file val:foo.c overridden by bar.c"
	ddcmd_err "module foo module baz =_	#! match-spec:module val:foo overridden by baz"
	ddcmd_err "format foo format bar =_	#! match-spec:format val:foo overridden by bar"
	ddcmd_err "class D2_CORE class D2_KMS +p #! match-spec:class val:D2_CORE overridden by D2_KMS"
	ddcmd_err "module foo +x	#! unknown flag 'x'"

	# line value errs
	ddcmd_err "line 10 line 20 +l	#! match-spec: line used 2x"
	ddcmd_err "line 10a +pl		#! bad line-number: 10a"
	ddcmd_err "line 100-10 +pf	#! last-line:10 < 1st-line:100"

	# colon-delimited syntax errs
	ddcmd_err "func foo file bar.c:func_bar =_	#! match-spec:func val:foo overridden by func_bar"
	ddcmd_err "file bar.c:100-10 +pf	#! last-line:10 < 1st-line:100"
	ddcmd_err "file bar.c:10a +pl		#! bad line-number: 10a"
	ddcmd_err "line 10-20 file bar.c:30 +pl	#! match-spec: line used 2x"
    done

    # Reset to default verbose level 0 at the end of basic errors
    echo 0 > /sys/module/dynamic_debug/parameters/verbose
    ddcmd =_
}

# these queries run against the builtin file: kernel/params.c, and change
# flags.  The control file state-of-interest is found by path $f,
# perfectly binding the stimulus scope to the observation slice.
function FT_basic_queries {
    v_echo "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires dynamic_debug built into kernel"
	return
    fi
    local f='kernel/params.c'
    ddcmd =_ # zero everything

    ddcmd "file $f +mf" "$f"
    ddcmd "file $f +l"  "$f"
    ddcmd "file $f -m"  "$f"
    ddcmd "file $f =_"  "$f"
}

function FT_multi_query {
    v_echo "${GREEN}# MULTI_QUERY_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires dynamic_debug built into kernel"
	return
    fi
    local f='kernel/params.c'
    ddcmd =_ # zero everything

    # multi-query commands on a single line, split on ;/@ respectively
    ddcmd "file $f +mf ; file $f func parse_args +sl" "$f"
    ddcmd "file $f -f @ file $f func parse_args -l"   "$f"

    # verify multi-cmd input, newline separated, with embedded comments
    ddcmd =_ # reset before multiline query to capture full transition
    ddcmd "file $f =_		# clear params
      file $f +ml		# set flags
      file $f func parse_args +fs # set other flags" \
	  "$f"

    # clear flags and verify
    ddcmd "file $f =_" "$f"
}

# test parsing on spaces, commas. testing against builtin file kernel/params.c
function FT_comma_terminators {
    v_echo "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires dynamic_debug built into kernel"
	return
    fi
    local f='kernel/params.c'
    ddcmd =_
    ddcmd "file $f +p" "$f"

    ddcmd "file,$f,=_" "$f"
    ddcmd "file,$f,+mf" "$f"
    # ignore empty tokens
    ddcmd ",file ,, ,  $f, -p" "$f"
    ddcmd " , file ,,, ,  $f, -m" "$f"

    ddcmd =_
}
# testing classmap-based query enablers and class configurations
function FT_test_classes {
    v_echo "${GREEN}# TEST_CLASSES - classmap-based query enablers and class configs ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # 1. Verify initial multi-query enablement state via file slice
    my_modprobe test_dynamic_debug \
        dyndbg="class D2_CORE,+pf;class D2_KMS,+ps;class D2_ATOMIC +pm"
    verify_control_slice '\[test_dynamic_debug\]'

    # 2. Verify state transition and live-printing end-to-end via ddcmd_load!
    ddcmd_load "class,D2_CORE,+pmf;class,D2_KMS,+pls;class,D2_ATOMIC,+pml" \
        '\[test_dynamic_debug\]' \
        "/sys/module/test_dynamic_debug/parameters/do_classes" "1"

    ifrmmod test_dynamic_debug
}
function FT_classmap_inheritance {
    v_echo "${GREEN}# TEST_MOD_SUBMOD - Classmap state inheritance between supermod and submod ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # 1. Load submod directly (which auto-loads supermod with default parameters)
    my_modprobe test_dynamic_debug_submod \
	"dyndbg=+p;class D2_CORE +pfs;class D2_KMS +pts;class D2_ATOMIC +pmf"
    verify_control_slice 'test_dynamic_debug'

    # 2. Runtime parameter changes to supermod propagate to submod descriptors
    set_param 0x57 /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    set_param 4 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice 'test_dynamic_debug'

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # 3. Pre-initialize supermod parameter state at load-time
    my_modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    verify_control_slice '\[test_dynamic_debug\]'

    # 4. Verify submod inherits pre-initialized supermod classmap parameter state upon load
    my_modprobe test_dynamic_debug_submod
    verify_control_slice 'test_dynamic_debug'

    # 5. Prove load-time (modprobe) and runtime (sysfs write) parameter equivalence
    local hash_modprobe=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    my_modprobe test_dynamic_debug
    my_modprobe test_dynamic_debug_submod
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 5 > /sys/module/test_dynamic_debug/parameters/p_level_num

    local hash_sysfs=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')
    if [ "$hash_modprobe" != "$hash_sysfs" ]; then
        echo -e "${RED}: Load-time vs runtime parameter equivalence check failed!${NC}"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: parameter load-time (modprobe) " \
            "and runtime (sysfs write) are equivalent!${NC}"
    fi
    # 6. End-to-end syslog content logging verification
    log_start
    echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    echo 1 > /sys/module/test_dynamic_debug_submod/parameters/do_classes
    log_stop

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
}

function FT_modprobe_w_param {
    v_echo "${GREEN}# TEST_MODPROBES ${NC}"
    local verbose

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    for verbose in 1 2; do # 3 4 0; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	# Verify each parameter load sequence with 100% DRY modularity
	verify_modprobe_param_logging "do_classes" "1"
	verify_modprobe_param_logging "do_bulk" "1"

	# Sequence composite bitmasks to verify disjoint bit transitions
	for mask in "0x05" "0x12" "0x1f" "0x00"; do
            verify_modprobe_param_logging "p_disjoint_bits" "$mask"
	done

	# Sequence levels to verify both growing and shrinking verbose transitions
	for lvl in "3" "5" "4" "0"; do
            verify_modprobe_param_logging "p_level_num" "$lvl"
	done
    done
    ddcmd =_
}

# Built-in Feature Tests (Can run on any CONFIG_DYNAMIC_DEBUG kernel, modular or monolithic)
builtin_tests=(
    FT_grammar_ok
    FT_grammar_errs
    FT_basic_queries
    FT_comma_terminators
    FT_multi_query
)

# Modular Feature Tests (Require CONFIG_MODULES=y and test_dynamic_debug*.ko available)
modular_tests=(
    FT_test_classes
    FT_classmap_inheritance
    FT_modprobe_w_param
)

# ==============================================================================
# GOLDEN_RECORDS (MD5 Fingerprint Verification Database)
#
# This database stores the expected invariant log content hashes for our tests.
# Since the key has the line-number of the callsite, we dont yet
# support looping over a test-call, maybe we'll need to address that
# later.
#
# NB: records have lineno of the test in code above. table at bottom
# means inserts dont shift test-lines.
#
# ==============================================================================
function GOLDEN_RECORDS {
    cat << 'EO_DYNDBG_GOLDEN_REFS' | {
#K= 95b249c7ec2cbd5a436f96060b2bee37 FT_grammar_errs.1
#K= 200c01632c52a63f6d186da1c6460740 FT_grammar_errs.2
#K= 7d7141900ce6e32f15c99202309c63a4 FT_grammar_errs.3
#K= 1bb798a5831d0119789d424ef6cb55c4 FT_grammar_errs.4
#K= 5edd66e308b2792d5694df86c07a3eaf FT_grammar_errs.5
#K= 6f87d92ffe0812550f43287127c6f2b9 FT_grammar_errs.6
#K= c0eb05b58a008c722e091e1ae74440ec FT_grammar_errs.7
#K= 911929ec0e2ffc1f13822b479dec6805 FT_grammar_errs.8
#K= 1ac52ce8ba553ec23eb70faa3ffa1197 FT_grammar_errs.9
#K= c1407512376369d2e591a4b25a4b607a FT_grammar_errs.10
#K= ea94add1d76d17dbb69c1b86f3aa42b2 FT_grammar_errs.11
#K= 2046abda72725ea06fe339d5f364f1c9 FT_grammar_errs.12
#K= b72f7fccf76f8a5bee47a05d7bb545fb FT_grammar_errs.13
#K= 98e2bd3e4f3da58536496a38ec3e6238 FT_grammar_errs.14
#K= b371c6ba52503d037dbc43da788af8be FT_grammar_errs.15
#K= cb8288d607b0c5282125852f3ab05107 FT_grammar_errs.16
#K= c2a8d0401e4cf85ccebedf61c5d0ff4b FT_grammar_errs.17
#K= cb8288d607b0c5282125852f3ab05107 FT_grammar_errs.18
#K= b371c6ba52503d037dbc43da788af8be FT_grammar_errs.19
#K= 98e2bd3e4f3da58536496a38ec3e6238 FT_grammar_errs.20
#K= c4e8b7261811e9fdf0c80fa9e374a29e FT_grammar_errs.21
#K= 51030cd01ed601a68dd48356902954b1 FT_grammar_errs.22
#K= af8818de076407d4e7f4e384ba110ac5 FT_grammar_errs.23
#K= 023ba04ffced833b4776fb4fcaae7755 FT_grammar_errs.24
#K= 0a082bb9d50f9a6257c3be5938758150 FT_grammar_errs.25
#K= 3b956344b2338de9dd0625391b41ed35 FT_grammar_errs.26
#K= 97779a5b22bb43912268caca0dc391d8 FT_grammar_errs.27
#K= c02a669a8b76e15f0dcced9c4aacc5da FT_grammar_errs.28
#K= 89822eac51e11b55c50c4e72a1203452 FT_grammar_errs.29
#K= 56a65b28955c743f18001d1b423356fb FT_grammar_errs.30
#K= ff1186fdf1ffe20c89fe38db596e75c2 FT_grammar_errs.31
#K= 10945c0e8920342903ffeeb0d601babd FT_grammar_errs.32
#K= 6a333eb2cfbb2c347b8fdfb06a979aa9 FT_grammar_errs.33
#K= 55dba10e65775dfd1e229ddf0eb2608e FT_grammar_errs.34
#K= dfa3b6908ea9f4b23005925d95dedd8e FT_grammar_errs.35
#K= 3cfcc96931e76ce6396da3e27c61a6ef FT_grammar_errs.36
#K= 04e37fe069eed91515c6a24301378548 FT_grammar_errs.37
#K= 130bf64b92a5e34d1c4cce98899084e1 FT_grammar_errs.38
#K= 9681a05658f1a50e3434aa991b5ce992 FT_grammar_errs.39
#K= 38506317363a188fbb7a417ff50457a9 FT_grammar_errs.40
#K= c4e8b7261811e9fdf0c80fa9e374a29e FT_grammar_errs.41
#K= 8c08a8b5cacbf885d022360ed856e7a1 FT_grammar_errs.42
#K= f0f4d894a49bd2c1856cf4f013f83e54 FT_grammar_errs.43
#K= 25b92d52cb299fa8d84c5584e490bd97 FT_grammar_errs.44
#K= 0a082bb9d50f9a6257c3be5938758150 FT_grammar_errs.45
#K= 2a41940f70efa80e39cb3356931e49af FT_grammar_errs.46
#K= a3cbfcf5e452fb11994fdf41254c6775 FT_grammar_errs.47
#K= d47387d5885927ac4d33f0675a721963 FT_grammar_errs.48
#K= 37d2984315284521b084e62feb889527 FT_grammar_errs.49
#K= 99b7d60f3afb7ab9fb8e14340c30df02 FT_grammar_errs.50
#K= a16f404c515c40418f43ea1d4d8e6a7b FT_grammar_errs.51
#K= 424841345d03bcfad704e9cdfce29794 FT_grammar_errs.52
#K= de2bf9687e8374f566db45d45274c313 FT_grammar_errs.53
#K= 6d923562c9fc7aaf1ac1c393aa7b55f9 FT_grammar_errs.54
#K= 704bfc82d4cf8fee52552d33bacfc7df FT_grammar_errs.55
#K= 426bf966b975ade7ec8c6ccf40463583 FT_grammar_errs.56
#K= 59a11ff06013a43e7bb6a21f8f55e7b7 FT_grammar_errs.57
#K= d6fde9bf3500a509fbd9cbf69fedb04f FT_grammar_errs.58
#K= c6a1064235d93bc17a623216b3817cb8 FT_grammar_errs.59
#K= 4042bdb2ea75ed254c429324b20c82b8 FT_grammar_errs.60
#K= 6e8599556a312200fb6d484565b6c52f FT_basic_queries.1
#K= db17180b59444e5e34a8fc20c40e4530 FT_basic_queries.2
#K= f7bd56bd407ff255ef2e3b5fb093ae6a FT_basic_queries.3
#K= 4ce44468b3b5f80ae42ade1f261b952f FT_basic_queries.4
#K= c8bbbac639f244ed9088208ebb7d83a6 FT_comma_terminators.1
#K= bed6350f058f600910837dae83a12f56 FT_comma_terminators.2
#K= 6e8599556a312200fb6d484565b6c52f FT_comma_terminators.3
#K= 68b329da9893e34099c7d8ad5cb9c940 FT_comma_terminators.4
#K= adc1ef04304c861c87fe294a8ba4e402 FT_comma_terminators.5
#K= 02e4fd94602e108cb89bfc70d47a5dad FT_multi_query.1
#K= c0fc482bbf18b9d5ffd6743b18bc34f7 FT_multi_query.2
#K= f03a7ca7316e8db4c0e16523dc41e75d FT_multi_query.3
#K= c518a50ba30ba8099d0dc874a27ecf16 FT_multi_query.4
#K= 8f18ea82c09460434b6e9e4cd12543e8 FT_test_classes.1
#K= a15ec4843acd721fbdfddc0b512c8032 FT_test_classes.2
#K= 20d4545f9753e677e72e3adf52527fd3 FT_test_classes.3
#K= 934d8677872fe26bd636a6c3d6416aa2 FT_classmap_inheritance.1
#K= cd1389958807063baa1ea4b06c61fa02 FT_classmap_inheritance.2
#K= 0708a283f0f1959135c797e36119e4af FT_classmap_inheritance.3
#K= 05f6efb80299d24cde65174f64d97308 FT_classmap_inheritance.4
#K= 7e92245008439ee79fe2460aeaa16a9b FT_classmap_inheritance.5
#K= 53d1b6875b65c79cfd759e209ae50b11 FT_modprobe_w_param.1
#K= 53d1b6875b65c79cfd759e209ae50b11 FT_modprobe_w_param.2
#K= 79d912e2aeb04dea70f9c7e701333f0a FT_modprobe_w_param.3
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.4
#K= 7cd75277388a6e7cf9c849442388f4af FT_modprobe_w_param.5
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.6
#K= 9196c43693f5e6059f3512e4d87e7347 FT_modprobe_w_param.7
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.8
#K= f9cee4512e5604603e0262fba4bea223 FT_modprobe_w_param.9
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.10
#K= b6a62433165f6423b81417f782551ab9 FT_modprobe_w_param.11
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.12
#K= de89753b843449d11da14240c4da4cad FT_modprobe_w_param.13
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.14
#K= 1d966d0cae735457791c93a86e9e1650 FT_modprobe_w_param.15
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.16
#K= 1aacb7c196a8354c46c60c73d78c6bf2 FT_modprobe_w_param.17
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.18
#K= e62014acf5ab6a76dfbfbcf67b2ca0e8 FT_modprobe_w_param.19
#K= e62014acf5ab6a76dfbfbcf67b2ca0e8 FT_modprobe_w_param.20
#K= 58c009cb287fa5df3f8c1c72c832cb42 FT_modprobe_w_param.21
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.22
#K= e7831be2aac4a82daba43966b9d31e19 FT_modprobe_w_param.23
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.24
#K= 4df2fbc0cae329debb14bedb5e7d86c0 FT_modprobe_w_param.25
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.26
#K= 7fa4c84490c42c750986614c3539d56a FT_modprobe_w_param.27
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.28
#K= 1eb866a813551061cd73178ba7833543 FT_modprobe_w_param.29
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.30
#K= a9e7424ed7b02696b5e12108971792dd FT_modprobe_w_param.31
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.32
#K= 5323d7746d983cdcfda4866255cd5123 FT_modprobe_w_param.33
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.34
#K= 7f961a7d3facd89bdd4b9d8e7b5541e9 FT_modprobe_w_param.35
#K= 6a320774e2b535ceb89d6cdde6f5d0fb FT_modprobe_w_param.36
EO_DYNDBG_GOLDEN_REFS
        # Read the K-recs and skip those for tests that can't run
        while read -r line; do
            # Filter built-in if needed
            if [ "${LACK_DD_BUILTIN:-0}" -eq 1 ]; then
                # Extract label (3rd field) from #K= line
                local label=$(echo "$line" | awk '{print $3}')
                if [[ "$label" == FT_basic_queries* \
			  || "$label" == FT_comma_terminators* \
			  || "$label" == FT_multi_query* ]]; then
                    continue
                fi
            fi
            # Filter modular if needed
            if [ "${LACK_TMOD:-0}" -eq 1 ]; then
                # Extract label (3rd field) from #K= line
                local label=$(echo "$line" | awk '{print $3}')
                if [[ "$label" == FT_test_classes* \
			  || "$label" == FT_classmap_inheritance* \
			  || "$label" == FT_modprobe_w_param* ]]; then
                    continue
                fi
            fi
            echo "$line"
        done
    }
}

# ==============================================================================
# Run tests

# Clear any stale seen/unregistered/drifted hashes from previous runs
: > "$SEEN_HASHES_FILE"
: > "$UNREG_HASHES_FILE"
: > "$DRIFT_HASHES_FILE"

ifrmmod test_dynamic_debug

# Check if loadable module support or our test modules are missing/builtin
LACK_TMOD=0
if [ -d "/sys/module/test_dynamic_debug" ]; then
    # If module is present but not in /proc/modules,
    # it is a builtin module (cannot unload/reload)
    if ! grep -q "^test_dynamic_debug " /proc/modules 2>/dev/null; then
        LACK_TMOD=1
    fi
else
    # Check if we can modprobe it from disk
    modprobe -q -n test_dynamic_debug || LACK_TMOD=1
fi

# 1. Run all Built-in Feature Tests
v_echo "${GREEN}# RUNNING BUILT-IN FEATURE TESTS ${NC}"
for test_func in "${builtin_tests[@]}"; do
    $test_func
    v_echo ""
done

# 2. Run Modular Feature Tests only if test modules are available
if [ $LACK_TMOD -eq 0 ]; then
    v_echo "${GREEN}# RUNNING MODULAR FEATURE TESTS ${NC}"
    for test_func in "${modular_tests[@]}"; do
        $test_func
        v_echo ""
    done
else
    v_echo "${YELLOW}# SKIPPING MODULAR TESTS: test_dynamic_debug.ko not available ${NC}"
fi

if [ "$V" -ge 1 ]; then
    echo -en "${GREEN}# Done on: "
    date
    echo -en "${NC}"
fi

audit_golden_records

# Output consolidated blocks of unregistered and drifted fingerprints
failed=0

if [ -s "$UNREG_HASHES_FILE" ]; then
    if [ "$K" -ne 2 ]; then
        echo -e "${YELLOW}\n# --- Unregistered Baselines ---"
        cat "$UNREG_HASHES_FILE"
        echo -e "# ------------------------------${NC}"
    fi
    : > "$UNREG_HASHES_FILE"
    failed=1
fi

if [ -s "$DRIFT_HASHES_FILE" ]; then
    if [ "$K" -ne 2 ]; then
        echo -e "${RED}\n# --- Drifted Baselines ---"
        cat "$DRIFT_HASHES_FILE"
        echo -e "# -------------------------${NC}"
    fi
    : > "$DRIFT_HASHES_FILE"
    failed=1
fi

if [ $failed -eq 1 ]; then
    [ "$K" -eq 1 ] && echo "fake success" && exit $ksft_pass
    [ "$K" -eq 2 ] && exit $ksft_pass
    exit $ksft_fail
fi

exit $ksft_pass

