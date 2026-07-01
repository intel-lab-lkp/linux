#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# description: Benchmark fetcharg performance (baseline vs kprobe vs fprobe vs eprobe)

DEBUG=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug|-d)
            DEBUG=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--debug|-d]"
            exit 1
            ;;
    esac
done

DEBUGFS_MOUNT=$(grep ^debugfs /proc/mounts | awk '{print $2}')
if [ -z "$DEBUGFS_MOUNT" ]; then
    mount -t debugfs nodev /sys/kernel/debug
    DEBUGFS_MOUNT="/sys/kernel/debug"
fi

TRACEFS_MOUNT=$(grep ^tracefs /proc/mounts | awk '{print $2}')
if [ -z "$TRACEFS_MOUNT" ]; then
    mount -t tracefs nodev /sys/kernel/tracing
    TRACEFS_MOUNT="/sys/kernel/tracing"
fi

MOD_NAME="fetcharg_bench"
MOD_FILE="./${MOD_NAME}.ko"

if [ ! -f "$MOD_FILE" ]; then
    echo "Module $MOD_FILE not found. Please run 'make' first."
    exit 1
fi

rmmod $MOD_NAME 2>/dev/null
insmod $MOD_FILE || { echo "Failed to load $MOD_FILE"; exit 1; }

TRIGGER_FILE="${DEBUGFS_MOUNT}/fetcharg_benchmark/trigger"

if [ ! -f "$TRIGGER_FILE" ]; then
    echo "Trigger file $TRIGGER_FILE not found."
    rmmod $MOD_NAME
    exit 1
fi

DYN_EVENTS="${TRACEFS_MOUNT}/dynamic_events"

# Helper to clear events
clear_events() {
    echo 0 > "${TRACEFS_MOUNT}/events/enable"
    echo > "$DYN_EVENTS"
}

run_bench() {
    if [ "$DEBUG" = "1" ]; then
        echo "=== [DEBUG] dynamic_events ===" >&2
        cat "$DYN_EVENTS" >&2
        echo "==============================" >&2
    fi
    cat "$TRIGGER_FILE"
}

calc_overhead() {
    local lps=$1
    local base_lps=$2
    if [ -z "$lps" ] || [ -z "$base_lps" ] || [ "$lps" = "-" ] || [ "$base_lps" = "-" ]; then
        echo "-"
        return
    fi
    awk -v lps="$lps" -v base_lps="$base_lps" 'BEGIN {
        if (lps == 0 || base_lps == 0) {
            print "-"
            exit
        }
        t = 1000000000.0 / lps
        t_base = 1000000000.0 / base_lps
        diff = t - t_base
        printf "%.2f ns", diff
    }'
}

echo "Running Fetcharg Micro Benchmark..."
echo "Please wait, this may take a few seconds..."

# Baseline
clear_events
baseline=$(run_bench)

# Kprobe
clear_events
echo "p:bench_kprobe fetcharg_bench_target" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/kprobes/bench_kprobe/enable"
kprobe_0=$(run_bench)

clear_events
echo "p:bench_kprobe fetcharg_bench_target a=\$arg1" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/kprobes/bench_kprobe/enable"
kprobe_1=$(run_bench)

clear_events
echo "p:bench_kprobe fetcharg_bench_target a=\$arg1 b=+0(+0(\$arg2)):u32" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/kprobes/bench_kprobe/enable"
kprobe_2=$(run_bench)

clear_events
echo "p:bench_kprobe fetcharg_bench_target a=\$arg1 b=+0(+0(\$arg2)):u32 c=+0(\$arg3):u32" \
    >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/kprobes/bench_kprobe/enable"
kprobe_3=$(run_bench)

# Fprobe
clear_events
echo "f:bench_fprobe fetcharg_bench_target" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/fprobes/bench_fprobe/enable"
fprobe_0=$(run_bench)

clear_events
echo "f:bench_fprobe fetcharg_bench_target a=\$arg1" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/fprobes/bench_fprobe/enable"
fprobe_1=$(run_bench)

clear_events
echo "f:bench_fprobe fetcharg_bench_target a=\$arg1 b=+0(+0(\$arg2)):u32" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/fprobes/bench_fprobe/enable"
fprobe_2=$(run_bench)

clear_events
echo "f:bench_fprobe fetcharg_bench_target a=\$arg1 b=+0(+0(\$arg2)):u32 c=+0(\$arg3):u32" \
    >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/fprobes/bench_fprobe/enable"
fprobe_3=$(run_bench)

# Eprobe
clear_events
echo "e:bench_eprobe fetcharg_bench/fetcharg_bench_event" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/eprobes/bench_eprobe/enable"
echo 1 > "${TRACEFS_MOUNT}/events/fetcharg_bench/fetcharg_bench_event/enable"
eprobe_0=$(run_bench)

clear_events
echo "e:bench_eprobe fetcharg_bench/fetcharg_bench_event a=\$a" >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/eprobes/bench_eprobe/enable"
echo 1 > "${TRACEFS_MOUNT}/events/fetcharg_bench/fetcharg_bench_event/enable"
eprobe_1=$(run_bench)

clear_events
echo "e:bench_eprobe fetcharg_bench/fetcharg_bench_event a=\$a b=+0(+0(\$b_ptr)):u32" \
    >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/eprobes/bench_eprobe/enable"
echo 1 > "${TRACEFS_MOUNT}/events/fetcharg_bench/fetcharg_bench_event/enable"
eprobe_2=$(run_bench)

clear_events
echo "e:bench_eprobe fetcharg_bench/fetcharg_bench_event a=\$a b=+0(+0(\$b_ptr)):u32 c=+0(\$c_ptr):u32" \
    >> "$DYN_EVENTS"
echo 1 > "${TRACEFS_MOUNT}/events/eprobes/bench_eprobe/enable"
echo 1 > "${TRACEFS_MOUNT}/events/fetcharg_bench/fetcharg_bench_event/enable"
eprobe_3=$(run_bench)

echo "--------------------------------------------------------------------------------"
echo "Configuration      0 Fetchargs       1 Fetcharg        2 Fetchargs        3 Fetchargs"
echo "--------------------------------------------------------------------------------"
printf "%-18s %15s %15s %18s %18s loops/sec\n" "Baseline" "$baseline" "-" "-" "-"
printf "%-18s %15s %15s %18s %18s overhead\n"  " "        "-" "-" "-" "-"
printf "%-18s %15s %15s %18s %18s loops/sec\n" \
    "Kprobe" "$kprobe_0" "$kprobe_1" "$kprobe_2" "$kprobe_3"
printf "%-18s %15s %15s %18s %18s overhead\n" " " \
    "$(calc_overhead $kprobe_0 $baseline)" \
    "$(calc_overhead $kprobe_1 $kprobe_0)" \
    "$(calc_overhead $kprobe_2 $kprobe_0)" \
    "$(calc_overhead $kprobe_3 $kprobe_0)"
printf "%-18s %15s %15s %18s %18s loops/sec\n" \
    "Fprobe" "$fprobe_0" "$fprobe_1" "$fprobe_2" "$fprobe_3"
printf "%-18s %15s %15s %18s %18s overhead\n" " " \
    "$(calc_overhead $fprobe_0 $baseline)" \
    "$(calc_overhead $fprobe_1 $fprobe_0)" \
    "$(calc_overhead $fprobe_2 $fprobe_0)" \
    "$(calc_overhead $fprobe_3 $fprobe_0)"
printf "%-18s %15s %15s %18s %18s loops/sec\n" \
    "Eprobe" "$eprobe_0" "$eprobe_1" "$eprobe_2" "$eprobe_3"
printf "%-18s %15s %15s %18s %18s overhead\n" " " \
    "$(calc_overhead $eprobe_0 $baseline)" \
    "$(calc_overhead $eprobe_1 $eprobe_0)" \
    "$(calc_overhead $eprobe_2 $eprobe_0)" \
    "$(calc_overhead $eprobe_3 $eprobe_0)"
echo "--------------------------------------------------------------------------------"

clear_events
rmmod $MOD_NAME
exit 0
