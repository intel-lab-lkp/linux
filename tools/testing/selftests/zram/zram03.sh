#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Test proactive-only swap allocation and pressure fallback.

set -eu

# shellcheck source=zram_lib.sh
. ./zram_lib.sh

TCID="zram03"
cg="/sys/fs/cgroup/zram-offload-$$"
cgroup_root="/sys/fs/cgroup"
ready="/tmp/zram-offload-ready-$$"
verified="/tmp/zram-offload-verified-$$"
allocator_pid=""
offload=""
zswap_enabled=""
zswap_writeback=""

fail()
{
	echo "$TCID: [FAIL] $*" >&2
	exit 1
}

skip()
{
	echo "$TCID: [SKIP] $*" >&2
	exit "$ksft_skip"
}

cleanup()
{
	set +e
	if [ -n "$allocator_pid" ]; then
		kill "$allocator_pid"
		wait "$allocator_pid"
	fi
	rm -f "$ready" "$verified"
	rmdir "$cg"
	cgroup_disable_memory_controller "$cgroup_root"
	if [ -n "$offload" ] && [ "$dev_makeswap" -lt "$dev_end" ]; then
		swapoff "$offload" >/dev/null 2>&1
	fi
	if [ "$dev_end" -ge "$dev_start" ]; then
		zram_cleanup
	fi
	if [ -n "$zswap_enabled" ]; then
		echo "$zswap_enabled" > /sys/module/zswap/parameters/enabled
	fi
}

swap_used_kb()
{
	awk -v device="$1" '$1 == device { print $4 }' /proc/swaps
}

zram_orig_data_size()
{
	awk '{ print $1 }' "/sys/block/${1##*/}/mm_stat"
}

wait_file()
{
	for _ in $(seq 1 400); do
		[ -e "$1" ] && return 0
		sleep 0.05
	done
	return 1
}

check_prereqs
# The feature marker also requires CONFIG_VM_EVENT_COUNTERS.
grep -q '^swpout_offload_refused ' /proc/vmstat ||
	skip "offload refusal counters are unavailable"
[ -x ./swap_offload ] || skip "swap_offload helper is unavailable"
[ -e /sys/fs/cgroup/cgroup.controllers ] || skip "cgroup v2 controllers are unavailable"
grep -qw memory /sys/fs/cgroup/cgroup.controllers || skip "memory controller is unavailable"
[ -e /sys/module/zswap/parameters/enabled ] || skip "zswap is unavailable"
zswap_enabled=$(cat /sys/module/zswap/parameters/enabled)

# Keep every reclaim transition on one CPU.  This makes the test exercise the
# cached-cluster provenance mismatch rather than relying on scheduler placement.
./swap_offload pin "$$" || skip "cannot pin test to one allowed CPU"

# Swap priorities are global. Put both test areas ahead of any existing swap
# so distro-managed zram or another high-priority area cannot absorb the test
# allocations and make the routing assertions fail spuriously.
max_prio=$(awk 'BEGIN { max = -1 } NR > 1 && $5 > max { max = $5 } END { print max }' /proc/swaps)
[ "$max_prio" -le 32765 ] || skip "cannot outrank existing swap priority $max_prio"
safe_prio=$((max_prio + 1))
offload_prio=$((max_prio + 2))

dev_num=2
zram_sizes="67108864 67108864"
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
zram_load
echo Y > /sys/module/zswap/parameters/enabled || skip "cannot enable zswap"
zram_set_disksizes

safe="/dev/zram${dev_start}"
offload="/dev/zram$((dev_start + 1))"
mkswap "$safe" >/dev/null
mkswap "$offload" >/dev/null
swapon -p "$safe_prio" "$safe"
dev_makeswap=$dev_start
./swap_offload reject-page-discard "$offload" "$offload_prio" ||
	fail "offload-only page discard was accepted"
./swap_offload accept-discard-once-pages "$offload" "$offload_prio" ||
	fail "resolved offload-only discard-once was rejected"
./swap_offload activate "$offload" "$offload_prio"
dev_makeswap=$dev_end

cgroup_enable_memory_controller "$cgroup_root" ||
	skip "cannot enable the cgroup v2 memory controller"
mkdir "$cg" || skip "cannot create test cgroup"
[ -e "$cg/memory.max" ] || skip "cgroup v2 memory controller is unavailable"
echo max > "$cg/memory.swap.max"
echo 1 > "$cg/memory.zswap.writeback" ||
	skip "cannot enable zswap writeback for test cgroup"
read -r zswap_writeback < "$cg/memory.zswap.writeback"
[ "$zswap_writeback" -eq 1 ] ||
	skip "an ancestor disables zswap writeback"

./swap_offload allocate 67108864 "$cg/cgroup.procs" "$ready" "$verified" &
allocator_pid=$!
wait_file "$ready" || fail "allocator did not become ready"

echo "16M swappiness=max" > "$cg/memory.reclaim" ||
	fail "proactive reclaim failed"
offload_before=$(swap_used_kb "$offload")
safe_before=$(swap_used_kb "$safe")
offload_orig=$(zram_orig_data_size "$offload")
[ "${offload_before:-0}" -gt 0 ] || fail "proactive reclaim missed offload area"
[ "${safe_before:-0}" -eq 0 ] || fail "proactive reclaim used lower-priority safe area"
[ "$((offload_orig + 1048576))" -ge "$((offload_before * 1024))" ] ||
	fail "zswap deferred the offload-only backend write"

echo 24M > "$cg/memory.max"
sleep 1
offload_pressure=$(swap_used_kb "$offload")
safe_pressure=$(swap_used_kb "$safe")
safe_orig=$(zram_orig_data_size "$safe")
[ "${safe_pressure:-0}" -gt 0 ] || fail "pressure reclaim missed safe fallback"
[ "$offload_pressure" -le "$offload_before" ] ||
	fail "pressure reclaim allocated offload-only slots"
[ "$safe_orig" -lt "$((safe_pressure * 1024))" ] ||
	fail "zswap did not retain any unmarked safe-area pages"

echo max > "$cg/memory.max"
echo "4M swappiness=max" > "$cg/memory.reclaim" ||
	fail "second proactive reclaim failed"
offload_after=$(swap_used_kb "$offload")
[ "$offload_after" -gt "$offload_pressure" ] ||
	fail "proactive priority was not restored after pressure reclaim"

swapoff "$offload" || fail "swapoff could not recover offloaded pages"
kill -USR1 "$allocator_pid" || fail "cannot request data verification"
wait_file "$verified" || fail "allocator did not verify recovered data"
kill -0 "$allocator_pid" || fail "allocator died during swapoff"
dev_makeswap=$dev_start

echo "$TCID: [PASS]"
