#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Launch ipv4_addr_lookup stress test inside virtme-ng
#
# Must be run from the kernel build tree root.
#
# Options:
#   --verbose       Show kernel console (vng boot messages) in real time.
#   --taskset CPUS  Pin the VM to specific CPUs via taskset.
#                   Example: --taskset 12-19  (pin to E-cores on i7-12800H)
#   --isolated      Run VM in bench.slice cgroup (proper CPU isolation).
#   --no-turbo      Disable turbo boost for stable CPU frequency.
#   --freq MHZ      Pin CPU frequency on bench CPUs (e.g. --freq 1200).
#                   Sets scaling_min_freq=scaling_max_freq for thermal stability.
#   All other options are forwarded to ipv4_addr_lookup_test.sh (see --help).
#
# bench.slice setup (required for --isolated):
#   The --isolated option uses a dedicated cgroup slice to pin the VM to
#   specific CPUs while keeping other system processes off those CPUs.
#   The script also sets cpuset.cpus.partition=isolated at runtime to
#   remove bench CPUs from the scheduler's load balancing domain
#   (similar to isolcpus= but reversible). Restored on exit.
#
#   One-time setup (as root, adjust CPU range for your system):
#
#     # Create the slice (example: reserve CPUs 12-19 for benchmarks)
#     systemctl set-property --runtime bench.slice AllowedCPUs=12-19
#
#     # Confine everything else to the remaining CPUs
#     systemctl set-property --runtime user.slice AllowedCPUs=0-11
#     systemctl set-property --runtime system.slice AllowedCPUs=0-11
#     systemctl set-property --runtime init.scope AllowedCPUs=0-11
#
#   To make persistent, drop the --runtime flag (writes to /etc/systemd).
#
# Examples (run from kernel tree root):
#   ./tools/testing/selftests/net/ipv4_addr_lookup_test_virtme.sh
#     --num-addrs 1000 --duration 10
#     --verbose --num-addrs 2000
#     --taskset 12-19 --num-addrs 10000   # pinned to E-cores
#     --isolated --num-addrs 10000         # proper cgroup isolation

set -eu

# Parse options consumed here (not forwarded to the inner test).
VERBOSE=""
TASKSET_CPUS=""
BENCH_SLICE=0
NO_TURBO=0
PIN_FREQ_KHZ=0
INNER_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose)  VERBOSE="--verbose"; INNER_ARGS+=("--verbose"); shift ;;
        --taskset)  TASKSET_CPUS="$2"; shift 2 ;;
        --isolated) BENCH_SLICE=1; shift ;;
        --no-turbo) NO_TURBO=1; shift ;;
        --freq)     PIN_FREQ_KHZ=$(( $2 * 1000 )); shift 2 ;;
        *)          INNER_ARGS+=("$1"); shift ;;
    esac
done
TEST_ARGS=""
[ ${#INNER_ARGS[@]} -gt 0 ] && TEST_ARGS=$(printf '%q ' "${INNER_ARGS[@]}")

if [ ! -f "vmlinux" ]; then
    echo "ERROR: virtme-ng needs vmlinux; run from a compiled kernel tree:" >&2
    echo "  cd /path/to/kernel && $0" >&2
    exit 1
fi

# Verify .config has the options needed for virtme-ng and this test.
KCONFIG=".config"
if [ ! -f "$KCONFIG" ]; then
    echo "ERROR: No .config found -- build the kernel first" >&2
    exit 1
fi

MISSING=""
for opt in CONFIG_VIRTIO CONFIG_VIRTIO_PCI CONFIG_VIRTIO_NET \
           CONFIG_VIRTIO_CONSOLE CONFIG_NET_9P CONFIG_NET_9P_VIRTIO \
           CONFIG_9P_FS CONFIG_VETH CONFIG_IP_MULTIPLE_TABLES; do
    if ! grep -q "^${opt}=[ym]" "$KCONFIG"; then
        MISSING+="  $opt\n"
    fi
done
if [ -n "$MISSING" ]; then
    echo "ERROR: .config is missing options required by virtme-ng:" >&2
    echo -e "$MISSING" >&2
    echo "Consider: vng --kconfig (or make defconfig + enable above)" >&2
    exit 1
fi

TESTDIR="tools/testing/selftests/net"
TESTNAME="ipv4_addr_lookup_test.sh"
LOGFILE="ipv4_addr_lookup_test.log"
LOGPATH="$TESTDIR/$LOGFILE"
CONSOLELOG="ipv4_addr_lookup_console.log"
rm -f "$LOGPATH" "$CONSOLELOG"

log_config() {
    echo "  Config: $*"
}

echo "Starting VM... test output in $LOGPATH, kernel console in $CONSOLELOG"

# earlycon on COM2 for reliable kernel console capture.
SERIAL_CONSOLE="earlycon=uart8250,io,0x2f8,115200"
SERIAL_CONSOLE+=" console=uart8250,io,0x2f8,115200"
CPU_PIN_CMD=""
if [ "$BENCH_SLICE" -eq 1 ]; then
    # bench.slice + systemd overrides confine all other processes to CPUs 0-11.
    # Move ourselves into bench.slice cgroup (user.slice blocks affinity to
    # CPUs 12-19), then use taskset. vng needs a PTY so systemd-run --scope
    # is not an option.
    BENCH_CPUS=$(systemctl show bench.slice -p AllowedCPUs --value 2>/dev/null)
    if [ -z "$BENCH_CPUS" ]; then
        echo "ERROR: bench.slice cgroup not configured." >&2
        echo "" >&2
        echo "One-time setup (adjust CPU range for your system):" >&2
        echo "  sudo systemctl set-property --runtime bench.slice AllowedCPUs=12-19" >&2
        echo "  sudo systemctl set-property --runtime user.slice AllowedCPUs=0-11" >&2
        echo "  sudo systemctl set-property --runtime system.slice AllowedCPUs=0-11" >&2
        echo "  sudo systemctl set-property --runtime init.scope AllowedCPUs=0-11" >&2
        echo "" >&2
        echo "Or use --taskset CPUS for simple pinning without isolation." >&2
        exit 1
    fi
    # Set partition to isolated: removes bench CPUs from scheduler load
    # balancing (like isolcpus= but reversible). Restore in EXIT trap.
    PARTITION_PATH="/sys/fs/cgroup/bench.slice/cpuset.cpus.partition"
    ORIG_PARTITION=""
    if [ -f "$PARTITION_PATH" ]; then
        ORIG_PARTITION=$(cat "$PARTITION_PATH")
        if [ "$ORIG_PARTITION" != "isolated" ]; then
            echo isolated | sudo tee "$PARTITION_PATH" >/dev/null 2>&1 || true
        fi
    fi
    log_config "bench.slice CPUs: $BENCH_CPUS (partition=isolated)"
    echo $$ | sudo tee /sys/fs/cgroup/bench.slice/cgroup.procs >/dev/null
    CPU_PIN_CMD="taskset -c $BENCH_CPUS"
elif [ -n "$TASKSET_CPUS" ]; then
    # Try taskset directly first. If it fails (e.g. user.slice excludes
    # the requested CPUs), move into bench.slice and retry.
    if ! taskset -cp "$TASKSET_CPUS" $$ >/dev/null 2>&1; then
        if [ -d /sys/fs/cgroup/bench.slice ]; then
            echo $$ | sudo tee /sys/fs/cgroup/bench.slice/cgroup.procs >/dev/null
            log_config "moved into bench.slice to reach CPUs $TASKSET_CPUS"
        else
            echo "ERROR: taskset to CPUs $TASKSET_CPUS failed and no bench.slice available" >&2
            exit 1
        fi
    fi
    log_config "taskset CPUs: $TASKSET_CPUS"
    CPU_PIN_CMD="taskset -c $TASKSET_CPUS"
fi

# Disable turbo boost for stable frequencies during benchmarks
TURBO_RESTORED=0
NO_TURBO_PATH="/sys/devices/system/cpu/intel_pstate/no_turbo"
ORIG_FREQS=()
cleanup() {
    # Restore CPU frequencies
    for entry in "${ORIG_FREQS[@]}"; do
        local cpu="${entry%%:*}" freq="${entry#*:}"
        echo "$freq" | sudo tee /sys/devices/system/cpu/cpu"$cpu"/cpufreq/scaling_max_freq >/dev/null 2>&1 || true
        echo "$freq" | sudo tee /sys/devices/system/cpu/cpu"$cpu"/cpufreq/scaling_min_freq >/dev/null 2>&1 || true
    done
    # Restore turbo boost
    if [ "$NO_TURBO" -eq 1 ] && [ -f "$NO_TURBO_PATH" ]; then
        echo 0 | sudo tee "$NO_TURBO_PATH" >/dev/null 2>&1 || true
    fi
    # Restore cpuset partition
    if [ -n "${ORIG_PARTITION:-}" ] && [ -f "${PARTITION_PATH:-}" ]; then
        echo "$ORIG_PARTITION" | sudo tee "$PARTITION_PATH" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if [ "$NO_TURBO" -eq 1 ]; then
    if [ -f "$NO_TURBO_PATH" ]; then
        echo 1 | sudo tee "$NO_TURBO_PATH" >/dev/null
        log_config "turbo boost disabled (will restore on exit)"
    else
        echo "WARN: $NO_TURBO_PATH not found, cannot disable turbo" >&2
    fi
fi

# Pin CPU frequency for thermal stability
if [ "$PIN_FREQ_KHZ" -gt 0 ]; then
    # Determine which CPUs to pin: bench.slice CPUs, --taskset CPUs, or all
    if [ -n "${BENCH_CPUS:-}" ]; then
        FREQ_CPUS="$BENCH_CPUS"
    elif [ -n "$TASKSET_CPUS" ]; then
        FREQ_CPUS="$TASKSET_CPUS"
    else
        echo "WARN: --freq without --isolated or --taskset, skipping" >&2
        PIN_FREQ_KHZ=0
    fi
    if [ "$PIN_FREQ_KHZ" -gt 0 ]; then
        # Expand CPU list (e.g. "12-15,18" -> "12 13 14 15 18")
        FREQ_CPU_LIST=""
        IFS=',' read -ra parts <<< "$FREQ_CPUS"
        for part in "${parts[@]}"; do
            if [[ "$part" == *-* ]]; then
                IFS='-' read -r a b <<< "$part"
                FREQ_CPU_LIST+=" $(seq "$a" "$b")"
            else
                FREQ_CPU_LIST+=" $part"
            fi
        done
        PIN_FREQ_MHZ=$((PIN_FREQ_KHZ / 1000))
        for cpu in $FREQ_CPU_LIST; do
            freq_dir="/sys/devices/system/cpu/cpu${cpu}/cpufreq"
            [ -d "$freq_dir" ] || continue
            orig=$(cat "$freq_dir/scaling_max_freq" 2>/dev/null) || continue
            ORIG_FREQS+=("${cpu}:${orig}")
            echo "$PIN_FREQ_KHZ" | sudo tee "$freq_dir/scaling_max_freq" >/dev/null 2>&1 || true
            echo "$PIN_FREQ_KHZ" | sudo tee "$freq_dir/scaling_min_freq" >/dev/null 2>&1 || true
        done
        log_config "CPU frequency pinned to ${PIN_FREQ_MHZ} MHz on CPUs: $FREQ_CPUS (will restore on exit)"
    fi
fi

echo "(VM is booting, please wait ~30s)"
set +e
$CPU_PIN_CMD vng $VERBOSE --cpus 4 --memory 2G \
    --rwdir "$TESTDIR" \
    --append "panic=5 loglevel=4 $SERIAL_CONSOLE" \
    --qemu-opts="-serial file:$CONSOLELOG" \
    --exec "cd $TESTDIR && \
        ./$TESTNAME $TEST_ARGS 2>&1 | \
        tee $LOGFILE; echo EXIT_CODE=\$? >> $LOGFILE"
VNG_RC=$?
set -e

echo ""
if [ "$VNG_RC" -ne 0 ]; then
    echo "***********************************************************"
    echo "* VM CRASHED -- kernel panic or BUG_ON (vng rc=$VNG_RC)"
    echo "***********************************************************"
    if [ -s "$CONSOLELOG" ] && \
       grep -qiE 'kernel BUG|BUG:|Oops:|panic|WARN' "$CONSOLELOG"; then
        echo ""
        echo "--- kernel backtrace ($CONSOLELOG) ---"
        grep -iE -A30 'kernel BUG|BUG:|Oops:|panic|WARN' \
            "$CONSOLELOG" | head -50
    else
        echo ""
        echo "Re-run with --verbose to see the kernel backtrace:"
        echo "  $0 --verbose ${INNER_ARGS[*]:-}"
    fi
    exit 1
elif [ ! -f "$LOGPATH" ]; then
    echo "No log file found -- VM may have crashed before writing output"
    exit 2
else
    echo "=== VM finished ==="
fi

# Show test results from the log
echo ""
if grep -q "^Results:" "$LOGPATH"; then
    grep "^Results:" "$LOGPATH"
fi
grep -E "^(PASS|FAIL):" "$LOGPATH" || true

# Scan console log for unexpected kernel warnings (even on clean exit)
if [ -s "$CONSOLELOG" ]; then
    WARN_PATTERN='kernel BUG|BUG:|Oops:|WARNING:|WARN_ON|rhashtable'
    WARN_LINES=$(grep -cE "$WARN_PATTERN" "$CONSOLELOG" 2>/dev/null) || WARN_LINES=0
    if [ "$WARN_LINES" -gt 0 ]; then
        echo ""
        echo "*** kernel warnings in $CONSOLELOG ($WARN_LINES lines) ***"
        grep -E "$WARN_PATTERN" "$CONSOLELOG" | head -20
    fi
fi

# Extract exit code from log
if grep -q "^EXIT_CODE=" "$LOGPATH"; then
    INNER_RC=$(grep "^EXIT_CODE=" "$LOGPATH" | tail -1 | cut -d= -f2)
    exit "$INNER_RC"
fi
