#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Reproduce offload-only swap leaking into the workingset refault heuristic.

set -eu

# shellcheck source=zram_lib.sh
. ./zram_lib.sh

TCID="zram04"
cg="/sys/fs/cgroup/zram-workingset-$$"
cgroup_root="/sys/fs/cgroup"
tmp="${TMPDIR:-/var/tmp}/zram-workingset-$$"
ready="$tmp/ready"
worker=""
tmp_fs=""
mglru=""
zswap_enabled=""

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
	[ -n "$worker" ] && kill "$worker"
	[ -n "$worker" ] && wait "$worker"
	rm -rf "$tmp"
	rmdir "$cg"
	cgroup_disable_memory_controller "$cgroup_root"
	[ "$dev_end" -ge "$dev_start" ] && zram_cleanup
	[ -n "$mglru" ] && echo "$mglru" > /sys/kernel/mm/lru_gen/enabled
	[ -n "$zswap_enabled" ] &&
		echo "$zswap_enabled" > /sys/module/zswap/parameters/enabled
}

wait_helper_ready()
{
	for _ in $(seq 1 400); do
		[ -e "$ready" ] && return 0
		worker_state=$(awk '{ print $3 }' "/proc/$worker/stat" \
			2>/dev/null || :)
		if ! kill -0 "$worker" 2>/dev/null ||
		   [ "$worker_state" = Z ]; then
			if wait "$worker"; then
				status=0
			else
				status=$?
			fi
			worker=""
			[ "$status" -eq 2 ] &&
				skip "workingset calibration was insufficient"
			fail "workingset helper exited with status $status before readiness"
		fi
		sleep 0.05
	done
	fail "workingset helper timed out before readiness"
}

check_prereqs
# The feature marker also requires CONFIG_VM_EVENT_COUNTERS.
grep -q '^swpout_offload_refused ' /proc/vmstat ||
	skip "offload refusal counters are unavailable"
[ -x ./swap_offload ] || skip "swap_offload helper is unavailable"
[ -x ./workingset_offload ] || skip "workingset helper is unavailable"
[ -e /sys/fs/cgroup/cgroup.controllers ] || skip "cgroup v2 is unavailable"
grep -qw memory /sys/fs/cgroup/cgroup.controllers ||
	skip "cgroup v2 memory controller is unavailable"
[ "$(awk 'END { print NR }' /proc/swaps)" -eq 1 ] ||
	skip "test requires no pre-existing swap"
tmp_fs=$(stat -f -c %T "${TMPDIR:-/var/tmp}") ||
	skip "cannot identify the test filesystem"
[ "$tmp_fs" != tmpfs ] ||
	skip "test files require a disk-backed filesystem"

[ -e /sys/kernel/mm/lru_gen/enabled ] &&
	mglru=$(cat /sys/kernel/mm/lru_gen/enabled)
[ -e /sys/module/zswap/parameters/enabled ] &&
	zswap_enabled=$(cat /sys/module/zswap/parameters/enabled)
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
[ -n "$mglru" ] && echo 0 > /sys/kernel/mm/lru_gen/enabled
[ -n "$zswap_enabled" ] && echo N > /sys/module/zswap/parameters/enabled

dev_num=1
zram_sizes="134217728"
zram_load
zram_set_disksizes
offload="/dev/zram${dev_start}"
mkswap "$offload" >/dev/null
./swap_offload activate "$offload" 1
dev_makeswap=$dev_end

cgroup_enable_memory_controller "$cgroup_root" ||
	skip "cannot enable the cgroup v2 memory controller"
mkdir "$cg" || skip "cannot create test cgroup"
[ -e "$cg/memory.max" ] || skip "cgroup v2 memory controller is unavailable"
echo max > "$cg/memory.max"
echo max > "$cg/memory.swap.max"
mkdir "$tmp"

./workingset_offload "$cg/cgroup.procs" "$tmp/target" "$tmp/filler" \
	"$ready" unused &
worker=$!
wait_helper_ready

# Shadow retention uses local LRU/slab statistics updated by memcg flushing.
# Check that the 64 MiB anonymous and 48 MiB file setup is visible before
# evicting file pages.
stats_ready=0
for _ in $(seq 1 50); do
	if awk '
		$1 == "active_anon" || $1 == "inactive_anon" { anon += $2 }
		$1 == "file" { file = $2 }
		END { exit !(anon >= 67108864 && file >= 50331648) }
	' "$cg/memory.stat"; then
		stats_ready=1
		break
	fi
	sleep 0.1
done
[ "$stats_ready" -eq 1 ] ||
	skip "initial working-set statistics did not become visible"

echo "48M swappiness=0" > "$cg/memory.reclaim" ||
	skip "file-only proactive reclaim failed"
kill -USR1 "$worker"
if wait "$worker"; then
	worker=""
	echo "$TCID: [PASS]"
	exit 0
else
	status=$?
fi
worker=""
[ "$status" -eq 2 ] && skip "workingset calibration was insufficient"
echo "$TCID: [FAIL] refaulted file pages were not activated" >&2
exit 1
