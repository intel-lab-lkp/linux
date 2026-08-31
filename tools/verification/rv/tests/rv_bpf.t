#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
source ../tests/engine.sh
test_begin

set_timeout 30s

RVDIR=/sys/kernel/tracing/rv

check "verify bpf subcommand help" \
	"$RV bpf --help" 0 "manage BPF monitor registration"

if $RV bpf unregister 2>&1 | grep -q "kernel does not support BPF monitors"; then
	test_end
	exit 0
fi

if ! mount | grep -q /sys/fs/bpf; then
	mount -t bpf bpf /sys/fs/bpf
fi

check "nohz does not exist before register" \
	"$RV mon nohz" 1 "monitor nohz does not exist"

check "tqueue does not exist before register" \
	"$RV mon tqueue" 1 "monitor tqueue does not exist"

check "register BPF monitors" \
	"$RV bpf register" 0 "registered [0-9]\+ BPF monitor(s)"

set_expected_timeout 2s

header="^[[:space:]]\+\(\([][A-Z_x<>-]\+\||\)[[:space:]]*\)\+$"
type="\(event\|error\)[[:space:]]\+"
genpid="[0-9]\+[[:space:]]\+"
selfpid="\$pid[[:space:]]\+"
cpu="\[[0-9]\{3\}\][[:space:]]\+"
state="[a-z_]\+ "
trace_task="${genpid}${cpu}${type}${genpid}${state}"
trace_task_self="${genpid}${cpu}${type}${selfpid}${state}"
trace_cpu="${genpid}${cpu}${type}${state}"
trace_cpu_self="${selfpid}${cpu}${type}${state}"

check_if_exists "run a BPF monitor without parameters" \
	"$RV mon nohz" "$RVDIR/monitors/nohz" "" "."

check_if_exists "run a per-task BPF monitor without parameters" \
	"$RV mon tqueue" "$RVDIR/monitors/tqueue" "" "."

check_if_exists "run per-task BPF monitor with tracing" \
	"$RV mon tqueue -t" "$RVDIR/monitors/tqueue" \
	"$header" "$trace_task_self" "\($header\|$trace_task\)"

check_if_exists "run per-task BPF monitor tracing also self" \
	"$RV mon tqueue -t -s" "$RVDIR/monitors/tqueue" \
	"$trace_task_self" "" "\($header\|$trace_task\)"

check_if_exists "run per-cpu BPF monitor with tracing" \
	"$RV mon nohz -t" "$RVDIR/monitors/nohz" \
	"$header" "$trace_cpu_self" "\($header\|$trace_cpu\)"

# This is unstable, we may never see events from self
#check_if_exists "run per-cpu BPF monitor tracing also self" \
#	"$RV mon nohz -t -s" "$RVDIR/monitors/nohz" \
#	"$trace_cpu_self" "" "\($header\|$trace_cpu\)"

lines_before=$(($(dmesg | wc -l) + 1))
check_if_exists "run a BPF monitor with a reactor" \
	"$RV mon nohz -r printk && dmesg | tail -n +$lines_before" "$RVDIR/monitors/nohz" \
	"rv: monitor nohz does not allow event"

set_timeout 30s

check "already enabled monitor returns error" \
	"echo 1 > $RVDIR/monitors/nohz/enable; $RV mon nohz" 1 \
	"monitor nohz (BPF) is already enabled"
[ -n "$TEST_COUNT" ] && echo 0 > $RVDIR/monitors/nohz/enable

check "already enabled monitor prevents unregistration" \
	"echo 1 > $RVDIR/monitors/nohz/enable; $RV bpf unregister" 1 \
	"monitor nohz is enabled, cannot unregister" \
	"unregistered [0-9]\+ BPF monitor(s)"
[ -n "$TEST_COUNT" ] && echo 0 > $RVDIR/monitors/nohz/enable

check "unregister BPF monitors" \
	"$RV bpf unregister" 0 "unregistered [0-9]\+ BPF monitor(s)"

check "nohz does not exist after unregister" \
	"$RV mon nohz" 1 "monitor nohz does not exist"

check "tqueue does not exist after unregister" \
	"$RV mon tqueue" 1 "monitor tqueue does not exist"

check "nothing left to unregister" \
	"$RV bpf unregister" 0 "unregistered 0 BPF monitor(s)"

# Error handling tests
check "bpf without subcommand" \
	"$RV bpf" 1 "usage: rv bpf"

check "invalid bpf subcommand" \
	"$RV bpf invalid" 1 "usage: rv bpf"

test_end
