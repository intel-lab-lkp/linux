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
    echo  "$query" "$range"
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

	ddcmd_err 'module foo format "parse +p'		# unclosed double quote

	# comments in queries tell the error in the logs
	ddcmd_err "module foo unknown_keyword value	# no flag err"
	ddcmd_err "module foo %pm	# bad flag-op "
	ddcmd_err "module foo +pfmHKDD	# bad flags after good "

	ddcmd_err "w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w 14 w15 w16 # too many tokens"
	ddcmd_err "func w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 +p # bad keyword w3"
	ddcmd_err "module foo line =_ # no line val"

	#
	ddcmd_err "func foo func bar =_		# func used 2x"
	ddcmd_err "module foo module baz =_	# module used 2x"
	ddcmd_err "class D2_CORE class D2_KMS +p # class used 2x"
	ddcmd_err "module foo +x		# unrecognized flag character"

	# line value errs
	ddcmd_err "line 10 line 20 +l		# line used 2x"
	ddcmd_err "line 10a +pl			# line value trailing garbage"
	ddcmd_err "line 100-10 +pf		# line range error (last < 1st)"
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

    # multi-query commands split on ; on a single line
    ddcmd "file $f +mf ; file $f func parse_args +sl" "$f"

    # verify multi-cmd input, newline separated, with embedded comments
    ddcmd =_ # reset before multiline query to capture full transition
    ddcmd "file $f =_		# clear params
      file $f +ml		# set flags
      file $f func parse_args +fs # set other flags" \
	  "$f"

    # clear flags and verify
    ddcmd "file $f =_" "$f"
}

function FT_path_module_queries {
    v_echo "${GREEN}# TEST_PATH_MODULE_QUERIES ${NC}"
    ddcmd =_

    # Find a module with a path/slash in its name from the control file
    local slashed_mod
    slashed_mod=$(awk -F'[][]' \
        '/^[^#:]+:[0-9]+/ { if ($2 ~ /\//) { print $2; exit } }' \
        /proc/dynamic_debug/control)

    if [ -z "$slashed_mod" ]; then
        echo "SKIP - no slashed module found to test paths"
        return
    fi

    local base_mod=$(basename "$slashed_mod")
    local slice_pattern="\[$slashed_mod\]"

    v_echo "# testing path module queries for module: $slashed_mod (basename: $base_mod)"

    # 1. Turn ON specific path
    ddcmd "module '$slashed_mod' +p"
    local hash_path=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Turn OFF using wide wildcard query
    ddcmd "module '*/$base_mod' =_"
    local hash_off=$(slice_and_hash_ddctrl "$slice_pattern")

    # 3. Turn ON using wide unscoped basename
    ddcmd "module '$base_mod' +p"
    local hash_base=$(slice_and_hash_ddctrl "$slice_pattern")

    # 4. Turn OFF using specific narrow path
    ddcmd "module '$slashed_mod' =_"

    if [ "$hash_path" != "$hash_base" ]; then
        echo -e "${RED}: Path vs Basename equivalence check failed! " \
            "Fingerprints do not match.${NC}"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: Slashed path and basename module queries match!${NC}"
    fi

    ddcmd =_
}

function FT_hyphen_underscore {
    v_echo "${GREEN}# TEST_HYPHEN_UNDERSCORE ${NC}"
    ddcmd =_

    # Find a module with an underscore in its name (e.g., from the control file)
    local mod_with_underscore
    mod_with_underscore=$(awk -F'[][]' \
        '/^[^#:]+:[0-9]+/ { if ($2 ~ /_/) { print $2; exit } }' \
        /proc/dynamic_debug/control)

    if [ -z "$mod_with_underscore" ]; then
        echo "SKIP - no module with underscore found in /proc/dynamic_debug/control"
        return
    fi

    local mod_with_hyphen=$(echo "$mod_with_underscore" | tr '_' '-')
    local base_underscore=$(basename "$mod_with_underscore")
    local base_hyphen=$(basename "$mod_with_hyphen")
    local slice_pattern="\[$mod_with_underscore\]"

    v_echo "# testing hyphen/underscore equivalence for module: $mod_with_underscore (hyphen: $mod_with_hyphen)"

    # 1. Enable using literal hyphen name, and record the state fingerprint
    v_echo "#   trying hyphen name: $mod_with_hyphen"
    ddcmd "module '$mod_with_hyphen' +p"
    local hash_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Disable and enable using underscore name, record the state fingerprint
    ddcmd =_
    v_echo "#   trying underscore name: $mod_with_underscore"
    ddcmd "module '$mod_with_underscore' +p"
    local hash_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore name equivalence!
    if [ "$hash_hyphen" != "$hash_underscore" ]; then
        echo -e "${RED}: Hyphen/Underscore equivalence check failed! " \
            "Fingerprints do not match.${NC}"
        echo -e "Hyphen name state hash:     $hash_hyphen"
        echo -e "Underscore name state hash: $hash_underscore"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: Hyphen/Underscore literal name equivalence matches!${NC}"
    fi

    # 3. Try kbasename with hyphen (if it has a path)
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        v_echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd "module '$base_hyphen' +pmf"
        local hash_base_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

        # Prove kbasename hyphen name matches literal path hyphen name (with different flags)!
        v_echo "#   trying full path hyphen with pmf flags"
        ddcmd =_
        ddcmd "module '$mod_with_hyphen' +pmf"
        local hash_path_pmf=$(slice_and_hash_ddctrl "$slice_pattern")
        if [ "$hash_path_pmf" != "$hash_base_hyphen" ]; then
            echo -e "${RED}: Hyphen kbasename check failed! " \
                "Fingerprints do not match full-path hyphen enablement.${NC}"
            exit $ksft_fail
        else
            v_echo "${GREEN}: Proven: Hyphen kbasename matches " \
                "full-path hyphen enablement!${NC}"
        fi
    fi

    # 4. Try kbasename with underscore
    ddcmd =_
    v_echo "#   trying underscore kbasename: $base_underscore"
    ddcmd "module '$base_underscore' +pmf"
    local hash_base_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore kbasename equivalence!
    if [ -n "$hash_base_hyphen" ] && [ "$hash_base_hyphen" != "$hash_base_underscore" ]; then
        echo -e "${RED}: Hyphen/Underscore kbasename equivalence check " \
            "failed! Fingerprints do not match.${NC}"
        exit $ksft_fail
    elif [ -n "$hash_base_hyphen" ]; then
        v_echo "${GREEN}: Proven: Hyphen/Underscore kbasename " \
            "equivalence matches!${NC}"
    fi

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
    ddcmd_load "class D2_CORE +pmf;class D2_KMS +pls;class D2_ATOMIC +pml" \
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
    FT_path_module_queries
    FT_hyphen_underscore
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
    cat << 'EOF' | {
#K= f3dbd5afb9aa1750f93275b634499e22 FT_grammar_errs.1
#K= 200c01632c52a63f6d186da1c6460740 FT_grammar_errs.2
#K= 7d7141900ce6e32f15c99202309c63a4 FT_grammar_errs.3
#K= 1bb798a5831d0119789d424ef6cb55c4 FT_grammar_errs.4
#K= 5edd66e308b2792d5694df86c07a3eaf FT_grammar_errs.5
#K= 6f87d92ffe0812550f43287127c6f2b9 FT_grammar_errs.6
#K= c0eb05b58a008c722e091e1ae74440ec FT_grammar_errs.7
#K= 911929ec0e2ffc1f13822b479dec6805 FT_grammar_errs.8
#K= c1407512376369d2e591a4b25a4b607a FT_grammar_errs.9
#K= 2046abda72725ea06fe339d5f364f1c9 FT_grammar_errs.10
#K= b72f7fccf76f8a5bee47a05d7bb545fb FT_grammar_errs.11
#K= 98e2bd3e4f3da58536496a38ec3e6238 FT_grammar_errs.12
#K= b371c6ba52503d037dbc43da788af8be FT_grammar_errs.13
#K= cb8288d607b0c5282125852f3ab05107 FT_grammar_errs.14
#K= 9346a310c4ad57cc3746afbace702c3e FT_grammar_errs.15
#K= 533d27af85eed3c0fd2eaec961982a36 FT_grammar_errs.16
#K= 114e0632585e205a3347c82bac7d79f2 FT_grammar_errs.17
#K= 73f5c173bafdfb9674b5ecce77db3354 FT_grammar_errs.18
#K= 0fc110d078f60eacdd389e5975ba18d9 FT_grammar_errs.19
#K= 815a1c52f365510c644450bb80c07e72 FT_grammar_errs.20
#K= 621e3cd81b553973cb40a935bb9298f1 FT_grammar_errs.21
#K= 581222901232344ade18bbda58302c48 FT_grammar_errs.22
#K= ea0aae3e01b3bb22eb8ad7acd327b371 FT_grammar_errs.23
#K= 8f28189bff62a3d5ed16f537d41a725a FT_grammar_errs.24
#K= 19c425e5d3a645b5dc5e23758ba0f4a1 FT_grammar_errs.25
#K= 75415542333f2250f0e060a54dae50f8 FT_grammar_errs.26
#K= 0955815c0e595ab2206e25aa31fe1ef2 FT_grammar_errs.27
#K= 3ff4c0b60db33e44c3cd6f0e14f81e3e FT_grammar_errs.28
#K= 9346a310c4ad57cc3746afbace702c3e FT_grammar_errs.29
#K= 7aaf0a16c287e66b62e11298ee160b34 FT_grammar_errs.30
#K= 06350c62105b537cdd0c67736b29727d FT_grammar_errs.31
#K= 6ef0ec01805c8719d828553098f95377 FT_grammar_errs.32
#K= 0fc110d078f60eacdd389e5975ba18d9 FT_grammar_errs.33
#K= 72203b2d88d0617cd5c659d3b80e26f9 FT_grammar_errs.34
#K= 6ecb03736d5cddb7ab2aaff49d561be9 FT_grammar_errs.35
#K= eaa989336cb7c96c13ef4a3964fc6898 FT_grammar_errs.36
#K= 5b624d9c133d7bb4f5370c3ce06929ed FT_grammar_errs.37
#K= 127275739b1fe04c84eedad28ec154f6 FT_grammar_errs.38
#K= 3e2fd15a7e8c0583bc5066524bc50508 FT_grammar_errs.39
#K= 781995971d28f732a792522f3c56cdd3 FT_grammar_errs.40
#K= 6614a677d9f9ac09d9825b4e989d2c42 FT_grammar_errs.41
#K= 70de9afed457a6be9f9c3c81cbd6d4d5 FT_grammar_errs.42
#K= 3c445fb23d701041e920a2a6d2b022c7 FT_basic_queries.1
#K= d8eb8f226860aa558fb8a98097f05be9 FT_basic_queries.2
#K= 6a86bb9209a3a0492bc6c2d29f4d5e52 FT_basic_queries.3
#K= 90804574a5336971465f92d6cc3aa7fb FT_basic_queries.4
#K= f2b4f24fece9c55f5a5d28323c2019f8 FT_basic_queries.5
#K= 8c2dd1164fbcefb721345ce62a864a37 FT_basic_queries.6
#K= 4542e1e5e7eadcbe8f90a9c934635618 FT_basic_queries.7
#K= 8146f6f983c3a76783c89b78b892ed48 FT_test_classes.1
#K= a15ec4843acd721fbdfddc0b512c8032 FT_test_classes.2
#K= 20d4545f9753e677e72e3adf52527fd3 FT_test_classes.3
#K= 3a6afd41fadaaf70fad18f8a4632ffec FT_classmap_inheritance.1
#K= 6f1e347f0a7930814d2e54d589439465 FT_classmap_inheritance.2
#K= a6e6aab5a6cb9a792d4296b1200ba409 FT_classmap_inheritance.3
#K= 315cc5dad3a775c54a0b60a0276300b7 FT_classmap_inheritance.4
#K= 7e92245008439ee79fe2460aeaa16a9b FT_classmap_inheritance.5
#K= 94610c57ac44bd7011002a654fd78f93 FT_modprobe_w_param.1
#K= 94610c57ac44bd7011002a654fd78f93 FT_modprobe_w_param.2
#K= c1309e18dc9bf2f57184fa13164d917d FT_modprobe_w_param.3
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.4
#K= a1232e658d95fbca8b23a69e9a0db965 FT_modprobe_w_param.5
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.6
#K= af7b3d532325b1c5ab990e4b32fed577 FT_modprobe_w_param.7
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.8
#K= 591411c42cf52d7c4c46d76bcc345a5f FT_modprobe_w_param.9
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.10
#K= b0435304108118e64529469e59332111 FT_modprobe_w_param.11
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.12
#K= 4d036833ce9f661057a4e13d97295c65 FT_modprobe_w_param.13
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.14
#K= 5c3c6ecf6a46f9ccebd60c5ca9ebdbb7 FT_modprobe_w_param.15
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.16
#K= 73a93377a823739e8aae44856a20fa7f FT_modprobe_w_param.17
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.18
#K= 10464b2c3e3972f05e93c609700f8fb2 FT_modprobe_w_param.19
#K= 10464b2c3e3972f05e93c609700f8fb2 FT_modprobe_w_param.20
#K= 07c1f81d5a58675a291dc77acd6938c4 FT_modprobe_w_param.21
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.22
#K= b070066b0eb13a033446bd05850b15e2 FT_modprobe_w_param.23
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.24
#K= 32ca47823c27e629e03c21aebfc25095 FT_modprobe_w_param.25
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.26
#K= 7b91db8e9f160aebb1ee87fab2232404 FT_modprobe_w_param.27
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.28
#K= e393499e02677de414e478f4e710eeb9 FT_modprobe_w_param.29
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.30
#K= 375e38613af3b49bb7c7689dfecf4177 FT_modprobe_w_param.31
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.32
#K= 745a61d20e26a6a22db0b99420fea80a FT_modprobe_w_param.33
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.34
#K= 3d2538bf868e71bff17c768cf118c352 FT_modprobe_w_param.35
#K= 13c54ef65e199294fd078ba72c0cd69a FT_modprobe_w_param.36
EOF
        # Read the K-recs and skip those for tests that can't run
        while read -r line; do
            # Filter built-in if needed
            if [ "${LACK_DD_BUILTIN:-0}" -eq 1 ]; then
                # Extract label (3rd field) from #K= line
                local label=$(echo "$line" | awk '{print $3}')
                if [[ "$label" == FT_basic_queries* \
			  || "$label" == FT_path_module_queries* \
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

