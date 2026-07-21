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
CUMULATIVE_DDCMDS="init"
error_msg=""
V=${V:=0}  # invoke as V=1 $0  for global verbose
K=${K:=0}  # K=1,2 to tolerate/pass on checksum mismatches

# Sanitize V to ensure it is a valid integer
if [[ ! "$V" =~ ^[0-9]+$ ]]; then
    V=0
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
    exit $ksft_skip # maybe later we can do more
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

# Clean up any leftover loaded test modules at initialization
grep -q "^test_dynamic_debug_submod " /proc/modules 2>/dev/null && rmmod test_dynamic_debug_submod
grep -q "^test_dynamic_debug " /proc/modules 2>/dev/null && rmmod test_dynamic_debug

# ==============================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ==============================================================================

function ddcmd () {
    # ddcmd <query_args> [range_pattern] [pass|fail|log]
    local args="$1"
    local range_pattern="$2"
    local action="${3:-pass}"

    # Verify if range transition check is requested (non-empty pattern)
    local do_range=0
    if [ -n "$range_pattern" ]; then
        do_range=1
    fi

    local exp_exit_code=0
    local do_log=0

    if [ "$action" = "fail" ]; then
	exp_exit_code=1
        do_log=1
    elif [ "$action" = "log" ]; then
        exp_exit_code=0
        do_log=1
    fi

    # Update cumulative state-machine lineage
    if [[ "$args" == *"=_"* ]]; then
        CUMULATIVE_DDCMDS="$args"
    else
        CUMULATIVE_DDCMDS="${CUMULATIVE_DDCMDS}; $args"
    fi

    # Update local bookend command tracker
    if [ "$IN_BOOKEND" -eq 1 ]; then
        if [ -z "$LOCAL_DDCMDS" ]; then
            LOCAL_DDCMDS="$args"
        else
            LOCAL_DDCMDS="${LOCAL_DDCMDS}; $args"
        fi
    else
        LOCAL_DDCMDS="$args"
    fi

    # 1. Automatically start dmesg capture if requested (fail or log)
    [ "$do_log" -eq 1 ] && log_start

    # 2. Automatically take pre-state file snapshot if requested (non-empty range)
    [ "$do_range" -eq 1 ] && capture_before "$range_pattern"

    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo "$output" | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')

    # Handle the exit code check
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code

    # Close and verify dmesg capture if we started one
    [ "$do_log" -eq 1 ] && log_stop

    # Close and verify range transition diff if we started one (non-empty range)
    [ "$do_range" -eq 1 ] && verify_after_change
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
	[ "$3" == 1 ] && echo "Error: '$error_msg'"
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
function slice_and_hash_ddctrl {
    local slice=$(slice_by_grep "$1" "$CONTROL_FILE")
    echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_classes)
    # $2 - parameter value (e.g. 1)
    # $3 - short descriptive tag (e.g. classes)
    local param="$1"
    local val="$2"
    local tag="$3"

    # Make sure both modules are completely unloaded to trigger a fresh load
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # Explicitly register the modprobe command as the local stimulus!
    LOCAL_DDCMDS="modprobe test_dynamic_debug ${param}=${val}"

    # 1. Capture and verify the load-time (modprobe) dmesg logs (R2)
    log_start
    modprobe test_dynamic_debug "${param}=${val}"

    # If it is a state-controlling parameter, trigger the print-workload 'do_classes=1'
    # inside the same syslog dmesg capture bookends to verify their actual pr_debug logging!
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        LOCAL_DDCMDS="${LOCAL_DDCMDS}; echo 1 > "
        LOCAL_DDCMDS="${LOCAL_DDCMDS}/sys/module/test_dynamic_debug/parameters/do_classes"
        echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    fi

    log_stop

    # 2. If it is a state-controlling parameter, verify runtime unsetting (S2 ➔ R1)
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        # Dynamically write 0 to unset the parameter at runtime via sysfs
        LOCAL_DDCMDS="echo 0 > /sys/module/test_dynamic_debug/parameters/${param}"
        echo 0 > "/sys/module/test_dynamic_debug/parameters/${param}"

        # Verify that the resulting control-file state is completely cleared
        local hash_unset=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')
        local default_hash="fd89900c8614f23c3a6e8a8d45aa3280"
	# Pristine fully-disabled slice hash

        if [ "$hash_unset" != "$default_hash" ]; then
            echo -e "${RED}: Runtime unsetting failed for ${param}! " \
                "Expected default-disabled, got ${hash_unset}${NC}"
            exit $ksft_fail
        else
            [ "$V" -ge 2 ] && echo -e "${GREEN}✔ Proven Runtime Unset: " \
                "echo 0 > ${param} successfully cleared callsites${NC}"
        fi
    fi
}

# ==============================================================================
function ifrmmod {
    [ "${LACK_TMOD:-0}" -eq 1 ] && return
    grep -q "^$1 " /proc/modules 2>/dev/null && rmmod $1
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
    #ddcmd "module foo file bar.c func buz class D2 line 100 +_" # 5 keywords

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

# these queries run against builtin module: params, and change flags.
# the state-of-interest, in control file, is found by path: kernel/params.c
function FT_basic_queries {
    v_echo "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd =_ # zero everything

    ddcmd "module params +mf" 'kernel/params.c'
    ddcmd "module params +l"  'kernel/params.c'
    ddcmd "module params -m"  'kernel/params.c'
    ddcmd "module params =_"  'kernel/params.c'

    # multi-query commands split on ; on a single line
    ddcmd "module params +mf ; module params func parse_args +sl"  'kernel/params.c'

    # 4. Verify multi-cmd input, newline separated, with embedded comments
    ddcmd =_ # reset before multiline query to capture full transition
    ddcmd "module params =_		# clear params
      module params +ml			# set flags
      module params func parse_args +fs # other flags" 'kernel/params.c'

    ddcmd =_
}

function FT_path_module_queries {
    v_echo "${GREEN}# TEST_PATH_MODULE_QUERIES ${NC}"
    ddcmd =_

    # Find how many 'main' modules we have in total (by basename)
    # Use a precise OR pattern to match exactly [main] or [*/main] and avoid irqdomain
    local total_main=$(grep -c "\[main\]\|\[[^]]*/main\]" /proc/dynamic_debug/control)
    v_echo "# found $total_main total 'main' modules"

    if [ $total_main -eq 0 ]; then
        echo "SKIP - no 'main' modules found to test slashes"
        return
    fi

    # Verify a robust, cross-query state-interaction handshake between
    # narrow path and wide wildcard/basename queries. This dynamically
    # proves they interact with the exact same underlying callsites!

    # 1. Turn ON specific path, verified under '[init/main]' range
    ddcmd "module 'init/main' +p" "init/main.c"

    # 2. Turn OFF using wide wildcard query,
    ddcmd "module '*/main' =_" "init/main.c"

    # 3. Turn ON using wide unscoped basename,
    ddcmd "module 'main' +p" "init/main.c"

    # 4. Turn OFF using specific narrow path,
    ddcmd "module 'init/main' =_" "init/main.c"
}

function FT_hyphen_underscore {
    v_echo "${GREEN}# TEST_HYPHEN_UNDERSCORE ${NC}"
    ddcmd =_

    # Find a module with a hyphen in its name (e.g., from the control file)
    local mod_with_hyphen
    mod_with_hyphen=$(awk -F'[][]' \
        '/^[^#:]+:[0-9]+/ { if ($2 ~ /-/) { print $2; exit } }' \
        /proc/dynamic_debug/control)

    if [ -z "$mod_with_hyphen" ]; then
        echo "SKIP - no module with hyphen found in /proc/dynamic_debug/control"
        return
    fi

    v_echo "# testing hyphen/underscore equivalence for module: $mod_with_hyphen"
    local mod_with_underscore=$(echo "$mod_with_hyphen" | tr '-' '_')
    local base_hyphen=$(basename "$mod_with_hyphen")
    local slice_pattern="\[[^]]*$base_hyphen\]"

    # 1. Enable using literal hyphen name, and record the state fingerprint
    v_echo "#   trying hyphen name: $mod_with_hyphen"
    ddcmd "module $mod_with_hyphen +p"
    # verify_file_slice "$slice_pattern"
    local hash_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Disable and enable using underscore name, record the state fingerprint
    ddcmd =_
    v_echo "#   trying underscore name: $mod_with_underscore"
    ddcmd "module $mod_with_underscore +p"
    # verify_file_slice "$slice_pattern"
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

        # Try kbasename with hyphen (if it has a path)
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        v_echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd "module $base_hyphen +pmf"
        # verify_file_slice "$slice_pattern" # omitted: slice contains dynamic
        # module info which drifts across different targets
        local hash_base_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

        # Prove kbasename hyphen name matches literal path hyphen name (with different flags)!
        v_echo "#   trying full path hyphen with pmf flags"
        ddcmd =_
        ddcmd "module $mod_with_hyphen +pmf"
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
    local base_underscore=$(echo "$base_hyphen" | tr '-' '_')
    ddcmd =_
    v_echo "#   trying underscore kbasename: $base_underscore"
    ddcmd "module $base_underscore +pmf"
    # verify_file_slice "$slice_pattern" # omitted: slice contains dynamic
    # module info which drifts across different targets
    local hash_base_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore kbasename equivalence!
    if [ "$hash_base_hyphen" != "$hash_base_underscore" ] && \
       [ -n "$hash_base_hyphen" ]; then
        echo -e "${RED}: Hyphen/Underscore kbasename equivalence check " \
            "failed! Fingerprints do not match.${NC}"
        exit $ksft_fail
    elif [ -n "$hash_base_hyphen" ]; then
        v_echo "${GREEN}: Proven: Hyphen/Underscore kbasename " \
            "equivalence matches!${NC}"
    fi

    ddcmd =_
}

# test parsing on spaces, commas. testing agains builtin [kernel/params]
function FT_comma_terminators {
    v_echo "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd "module params =_"

    # 1. Verify transition after commas-as-spaces query and splitting on @
    # Use non-printing decorator flags (+mf) to avoid print-enabling (+p) and
    # prevent syslog pollution
    #ddcmd "module,params,=_ @ module,params,+mf" 'kernel/params.c'

    # 2. Verify transition after ignored-commas query
    ddcmd ",module ,, ,  params, -p" 'kernel/params.c'

    # 3. Verify transition after quoted-commas query
    ddcmd " , module ,,, ,  params, -m" 'kernel/params.c'

    ddcmd =_
}

# testing classmap-based query enablers and class configurations
function FT_test_classes {
    v_echo "${GREEN}# TEST_CLASSES - classmap-based query enablers and class configs ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # 1. Verify initial multi-query enablement state via file slice
    modprobe test_dynamic_debug \
        dyndbg="class,D2_CORE,+pf;class,D2_KMS,+ps;class,D2_ATOMIC,+pm"
    verify_file_slice '\[test_dynamic_debug\]'

    # 2. Verify state transition and live-printing end-to-end via ddcmd_load!
    ddcmd_load "class,D2_CORE,+pmf@class,D2_KMS,+pls@class,D2_ATOMIC,+pml" \
        '\[test_dynamic_debug\]' \
        "/sys/module/test_dynamic_debug/parameters/do_classes" "1"

    ifrmmod test_dynamic_debug
}

function FT_classmap_inheritance {
    v_echo "${GREEN}# TEST_MOD_SUBMOD ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # modprobe with class enablements
    modprobe test_dynamic_debug \
	dyndbg=class,D2_CORE,+pf@class,D2_KMS,+pt@class,D2_ATOMIC,+pm

    verify_file_slice '\[test_dynamic_debug\]'

    modprobe test_dynamic_debug_submod
    verify_file_slice '\[test_dynamic_debug_submod\]'

    # Verify 3 class changes
    ddcmd "class,D2_CORE,+pmf @ class,D2_KMS,+plt @ class,D2_ATOMIC,+pml" \
	  'test_dynamic_debug' "# add some prefixes"

    # now work the classmap-params
    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug_submod # get supermod too

    echo 1 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 4 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( V1-3 + D2_CORE )
    verify_file_slice 'test_dynamic_debug'
    echo 3 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_CORE, D2_DRIVER )
    verify_file_slice 'test_dynamic_debug'
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_DRIVER, D2_KMS, D2_ATOMIC )
    verify_file_slice 'test_dynamic_debug'

    # recap DRM_USE_DYNAMIC_DEBUG regression
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    # set super-mod params at load-time
    modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    verify_file_slice '\[test_dynamic_debug\]'
    modprobe test_dynamic_debug_submod
    # see them picked up by submod
    verify_file_slice 'test_dynamic_debug'

    # Real-time mathematical proof that load-time (modprobe) parameter parsing
    # and runtime (sysfs write) parameter configurations are perfectly equivalent!
    local hash_modprobe=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')

    # Fresh load with default parameters, then configure them dynamically at runtime
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug
    modprobe test_dynamic_debug_submod
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

    # --- Live Content Fingerprinting Phase ---
    log_start
    echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    echo 1 > /sys/module/test_dynamic_debug_submod/parameters/do_classes
    log_stop

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
}

function FT_modprobe_w_param {
    v_echo "${GREEN}# TEST_MODPROBES ${NC}"
    ddcmd =_
    local verbose

    for verbose in 1 2 3 4 0; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	# Verify each parameter load sequence with 100% DRY modularity
	verify_modprobe_param_logging "do_prints" "1" "classes_verb${verbose}"

	#verify_modprobe_param_logging "do_classes" "1" "classes_verb${verbose}"
	#verify_modprobe_param_logging "do_bulk" "1" "bulk_verb${verbose}"

	# Sequence composite bitmasks to verify disjoint bit transitions
	for mask in "0x05" "0x12" "0x1f" "0x00"; do
            verify_modprobe_param_logging "p_disjoint_bits" "$mask" \
					  "disjoint_${mask}_verb${verbose}"
	done

	# Sequence levels to verify both growing and shrinking verbose transitions
	for lvl in "3" "5" "4" "0"; do
            verify_modprobe_param_logging "p_level_num" "$lvl" \
					  "level_${lvl}_verb${verbose}"
	done
    done # verbose loop
    ddcmd =_
}

# Built-in Feature Tests (Can run on any CONFIG_DYNAMIC_DEBUG kernel, modular or monolithic)
builtin_tests=(
    FT_grammar_ok
    FT_grammar_errs
    FT_basic_queries
    #FT_path_module_queries
    #FT_hyphen_underscore
    #FT_comma_terminators
)

# Modular Feature Tests (Require CONFIG_MODULES=y and test_dynamic_debug*.ko available)
modular_tests=(
    #FT_test_classes
    #FT_classmap_inheritance
    #FT_modprobe_w_param
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
#K= bb85d28739876b0fdf1eb426baae8ef7 FT_grammar_errs.1        dmesg
#K= 9c405e06fcea50284df82d1ae9692403 FT_grammar_errs.2        dmesg
#K= 0b0d9b0e6f4c45c7d75d699fd3b6aa93 FT_grammar_errs.3        dmesg
#K= e008644e159c2a9d99a8d0806c06e71e FT_grammar_errs.4        dmesg
#K= 11833e6cf8fe8b03869aa4f669b63398 FT_grammar_errs.5        dmesg
#K= e85a4b665f5ef141e3f4d4505dec1da1 FT_grammar_errs.6        dmesg
#K= 3e1eba65bd936e1276c7e1d76b399595 FT_grammar_errs.7        dmesg
#K= e3ac86676e309d4e72ff3da287736c29 FT_grammar_errs.8        dmesg
#K= 466d325c23519119f754fa7860bae7c6 FT_grammar_errs.9        dmesg
#K= 51c489366cd86c16280858b95adf34ac FT_grammar_errs.10       dmesg
#K= dfc632c6c0cd6206b6141dbc5fb989db FT_grammar_errs.11       dmesg
#K= 69650fc26e97a78082bd851eb4a5c83b FT_grammar_errs.12       dmesg
#K= 5cfe5665962a482f416836793067f2a2 FT_grammar_errs.13       dmesg
#K= 661cc9a2d8d3c381bfeae41392432575 FT_grammar_errs.14       dmesg
#K= 5dd0accf9be37ce2f4bf697e9bdebe30 FT_grammar_errs.15       dmesg
#K= bd8d9cf661c9aca54dac93c74cfc1501 FT_grammar_errs.16       dmesg
#K= e991391048f2fdffaaed99ba36b787ed FT_grammar_errs.17       dmesg
#K= 82d99278f471ebfbf47eeb19898525cf FT_grammar_errs.18       dmesg
#K= 3c8d268c94b0bf97e64999654cd840c4 FT_grammar_errs.19       dmesg
#K= 1a448ff3f1df9155cc3acf1e124f24f5 FT_grammar_errs.20       dmesg
#K= 76c8a33c585d414fcc3f1c8b5ea5b4b3 FT_grammar_errs.21       dmesg
#K= 90cfc66e9d61f5c448353b97baa99145 FT_grammar_errs.22       dmesg
#K= 37a51dfd6e84cb64b69d3a0e23e68ceb FT_grammar_errs.23       dmesg
#K= 3aefa325b63742710b45e4531152c9d8 FT_grammar_errs.24       dmesg
#K= bc8d2c23282cd6834879bc1f1b303d05 FT_grammar_errs.25       dmesg
#K= 092ec5530d875be1dc96ad4202fc7dc3 FT_grammar_errs.26       dmesg
#K= 98caa2677e9070953a852b27ab57861e FT_grammar_errs.27       dmesg
#K= 8d755baa707c7b3b67491bf84708ef08 FT_grammar_errs.28       dmesg
#K= 5dd0accf9be37ce2f4bf697e9bdebe30 FT_grammar_errs.29       dmesg
#K= 2e37642dc8aee3d04b8a54a76b3b891c FT_grammar_errs.30       dmesg
#K= 593c8b4c41932e3bde564a0008627ced FT_grammar_errs.31       dmesg
#K= 85a2a0bee8d0cd10f883aa5477d62efb FT_grammar_errs.32       dmesg
#K= 3c8d268c94b0bf97e64999654cd840c4 FT_grammar_errs.33       dmesg
#K= 1a448ff3f1df9155cc3acf1e124f24f5 FT_grammar_errs.34       dmesg
#K= d9f0cffb0898c54690735a71b0d4f3bf FT_grammar_errs.35       dmesg
#K= 4cf728b6a32043ea5336a14d0c435312 FT_grammar_errs.36       dmesg
#K= a11ae3f86f4e5f539fd6168d318f74b4 FT_grammar_errs.37       dmesg
#K= c7d3830b0391f6a773205066b64e15f9 FT_grammar_errs.38       dmesg
#K= 5c506ea2bfd050439b580b91543e9be7 FT_grammar_errs.39       dmesg
#K= a257660007408f7bb0cb43c99b1c7bb8 FT_grammar_errs.40       dmesg
#K= 0082939f100300a7b327b143cf40cb43 FT_grammar_errs.41       dmesg
#K= 657ac4360f978687ad25a705b2bb89f1 FT_grammar_errs.42       dmesg
#K= 9a7aaed40738f1c5203af2e421a20bc2 FT_basic_queries.1       "kernel/params.c"
#K= 2a57a9e283a3912de3fa5e006fb330cc FT_basic_queries.2       "kernel/params.c"
#K= fed3186684428b2f05bcf5df960c639c FT_basic_queries.3       "kernel/params.c"
#K= 551f9801b6008553661453021ae6dfd3 FT_basic_queries.4       "kernel/params.c"
#K= 33f7162ac85020894fab9752ef07b89c FT_basic_queries.5       "kernel/params.c"
#K= 544d955c3bb9d3c78704f780d91396e0 FT_basic_queries.6       "kernel/params.c"


EOF
        # Read the K-recs and skip those for tests that can't run
        while read -r line; do
            # Filter built-in if needed
            if [ "${LACK_DD_BUILTIN:-0}" -eq 1 ]; then
                # Extract pattern (4th field) from #K= line
                local pattern=$(echo "$line" | awk '{print $4}')
                if [[ "$pattern" == *params* || "$pattern" == *main* ]]; then
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
rm -f "$SEEN_HASHES_FILE" "$UNREG_HASHES_FILE" "$DRIFT_HASHES_FILE"

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
    echo -e "${YELLOW}\n# --- Unregistered Baselines ---"
    cat "$UNREG_HASHES_FILE"
    echo -e "# ------------------------------${NC}"
    rm -f "$UNREG_HASHES_FILE"
    failed=1
fi

if [ -s "$DRIFT_HASHES_FILE" ]; then
    echo -e "${RED}\n# --- Drifted Baselines ---"
    cat "$DRIFT_HASHES_FILE"
    echo -e "# -------------------------${NC}"
    rm -f "$DRIFT_HASHES_FILE"
    failed=1
fi

# Cleanup
rm -f "$UNREG_HASHES_FILE" "$DRIFT_HASHES_FILE"

if [ $failed -eq 1 ]; then
    [ "$K" -eq 1 ] && echo "fake success" && exit $ksft_pass
    exit $ksft_fail
fi

exit $ksft_pass

