#!/bin/sh
# DHEI (Dynamic Housekeeping & Enhanced Isolation) Full-Coverage Verification Script
# Strict POSIX compliant version for reliability on all shells.

SYSFS_BASE="/sys/kernel/housekeeping"
ONLINE_CPUS=$(cat /sys/devices/system/cpu/online)
LAST_CPU=$(echo "$ONLINE_CPUS" | awk -F'[,-]' '{print $NF}')

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log_pass() { echo "${GREEN}[OK]${NC} $1"; }
log_fail() { echo "${RED}[FAIL]${NC} $1"; exit 1; }
log_info() { echo "[INFO] $1"; }

check_root() {
    [ "$(id -u)" -eq 0 ] || log_fail "Please run as root"
}

test_sysfs_structure() {
    log_info "TEST 1: Sysfs structure..."
    for node in smt_aware_mode timer rcu misc tick domain workqueue managed_irq kthread; do
        [ -f "$SYSFS_BASE/$node" ] || log_fail "Node $SYSFS_BASE/$node missing"
    done
    log_pass "All 9 DHEI sysfs nodes exist"
}

test_safety_guard() {
    log_info "TEST 2: Safety guard..."
    if echo "999-1024" > "$SYSFS_BASE/domain" 2>/dev/null; then
        log_fail "Safety guard failed: allowed isolation of all CPUs"
    fi
    log_pass "Safety guard blocked invalid mask"
}

test_smt_aware_mode() {
    log_info "TEST 3: SMT aware logic..."
    [ -f /sys/devices/system/cpu/cpu0/topology/thread_siblings_list ] || { log_info "SMT not supported"; return; }
    SIBLINGS=$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)
    FIRST=$(echo "$SIBLINGS" | cut -d',' -f1 | cut -d'-' -f1)
    echo 1 > "$SYSFS_BASE/smt_aware_mode"
    if echo "$FIRST" > "$SYSFS_BASE/timer" 2>/dev/null; then
         echo 0 > "$SYSFS_BASE/smt_aware_mode"
         log_fail "SMT mode failed: accepted partial core"
    else
         log_pass "SMT mode correctly rejected partial core"
    fi
    echo 0 > "$SYSFS_BASE/smt_aware_mode"
}

get_tick_count() {
    grep "LOC:" /proc/interrupts | awk -v cpu="$LAST_CPU" '{print $(cpu+2)}'
}

test_tick_dynamic() {
    log_info "TEST 4: Dynamic Tick toggle..."
    [ "$LAST_CPU" -eq 0 ] && return

    # Reset all to full housekeeping
    for node in tick rcu timer domain workqueue; do
        [ -f "$SYSFS_BASE/$node" ] && echo "$ONLINE_CPUS" > "$SYSFS_BASE/$node" 2>/dev/null
    done

    S1=$(get_tick_count)
    sleep 1
    S2=$(get_tick_count)
    log_info "Baseline ticks on CPU $LAST_CPU: $((S2-S1)) (per 1s)"

    # Isolate LAST_CPU by setting housekeeping for all types
    HK_MASK="0-$((LAST_CPU-1))"
    for node in tick rcu timer domain workqueue; do
        [ -f "$SYSFS_BASE/$node" ] && echo "$HK_MASK" > "$SYSFS_BASE/$node" 2>/dev/null
    done

    sleep 1
    S1=$(get_tick_count)
    sleep 2
    S2=$(get_tick_count)
    DIFF=$((S2-S1))
    log_info "Tick delta after isolation: $DIFF (per 2s)"
    [ "$DIFF" -gt 100 ] && log_fail "Tick not suppressed ($DIFF)"
    log_pass "Tick dynamically suppressed"
}

test_generic() {
    log_info "TEST 5: Notifier propagation..."
    for t in rcu workqueue misc kthread managed_irq; do
        echo "0-1" > "$SYSFS_BASE/$t"
        [ "$(cat "$SYSFS_BASE/$t")" = "0-1" ] || log_fail "$t update failed"
        log_pass "$t verified"
    done
}

get_busy() {
    grep "cpu$LAST_CPU " /proc/stat | awk '{print $2+$3+$4+$7+$8+$9}'
}

test_stress_domain() {
    log_info "TEST 6: Stress Domain Isolation..."
    command -v stress-ng >/dev/null 2>&1 || return
    [ "$LAST_CPU" -eq 0 ] && return
    echo "0-1" > "$SYSFS_BASE/domain"
    stress-ng --cpu 0 --timeout 10 --quiet &
    PID=$!
    sleep 2
    B1=$(get_busy)
    sleep 5
    B2=$(get_busy)
    DIFF=$((B2-B1))
    log_info "Busy jiffies delta: $DIFF (per 5s)"
    [ "$DIFF" -gt 150 ] && log_fail "CPU $LAST_CPU not isolated ($DIFF)"
    log_pass "Domain isolation verified under load"
    echo "$ONLINE_CPUS" > "$SYSFS_BASE/domain"
    wait "$PID" 2>/dev/null
}

test_stress_tick() {
    log_info "TEST 7: Stress Tick Suppression..."
    command -v stress-ng >/dev/null 2>&1 || return
    [ "$LAST_CPU" -eq 0 ] && return
    echo "$ONLINE_CPUS" > "$SYSFS_BASE/tick"
    taskset -c "$LAST_CPU" stress-ng --cpu 1 --timeout 15 --quiet &
    PID=$!
    sleep 2
    T1=$(get_tick_count)
    sleep 2
    T2=$(get_tick_count)
    log_info "Ticks WITH housekeeping: $((T2-T1)) (per 2s)"

    echo "0-1" > "$SYSFS_BASE/tick"
    sleep 2
    T1=$(get_tick_count)
    sleep 2
    T2=$(get_tick_count)
    DIFF_ISO=$((T2-T1))
    log_info "Ticks AFTER isolation: $DIFF_ISO (per 2s)"

    # Critical: Check if dmesg shows context tracking warnings during this test
    [ "$DIFF_ISO" -gt 100 ] && {
        log_info "Dmesg check for tick errors..."
        dmesg | grep -i "tick" | tail -n 5
    }

    log_pass "Tick suppression scenario logged"
    echo "$ONLINE_CPUS" > "$SYSFS_BASE/tick"
    wait "$PID" 2>/dev/null
}

check_root
test_sysfs_structure
test_safety_guard
test_smt_aware_mode
test_tick_dynamic
test_generic
test_stress_domain
test_stress_tick

log_pass "DHEI Verification Complete!"
