#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Test retained offload-only entries under ordinary and proactive reclaim.

set -eu

# shellcheck source=zram_lib.sh
. ./zram_lib.sh

TCID="zram05"
cg="/sys/fs/cgroup/zram-retained-$$"
cgroup_root="/sys/fs/cgroup"
tmp="${TMPDIR:-/var/tmp}/zram-retained-$$"
ready="$tmp/ready"
touched="$tmp/touched"
verified="$tmp/verified"
holder_pid=""
safe=""
safe_active=0
offload_backing=""
offload=""
offload_active=0
dm_name="zram-retained-$$"
dm_active=0
dm_node_created=0
zswap_enabled=""
thp_size=0
thp_kib=0
thp_sectors=0
page_kib=0

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
	if [ -n "$holder_pid" ]; then
		kill "$holder_pid"
		wait "$holder_pid"
	fi
	if [ "$offload_active" -eq 1 ]; then
		swapoff "$offload" >/dev/null 2>&1
	fi
	if [ "$safe_active" -eq 1 ]; then
		swapoff "$safe" >/dev/null 2>&1
	fi
	if [ "$dm_active" -eq 1 ]; then
		dmsetup --noudevsync --noudevrules remove "$dm_name" \
			>/dev/null 2>&1
	fi
	if [ "$dm_node_created" -eq 1 ]; then
		rm -f "$offload"
	fi
	rm -rf "$tmp"
	rmdir "$cg"
	cgroup_disable_memory_controller "$cgroup_root"
	if [ "$dev_end" -ge "$dev_start" ]; then
		zram_cleanup
	fi
	if [ -n "$zswap_enabled" ]; then
		echo "$zswap_enabled" > /sys/module/zswap/parameters/enabled
	fi
}

wait_file()
{
	for _ in $(seq 1 400); do
		[ -e "$1" ] && return 0
		if [ -n "$holder_pid" ]; then
			holder_state=$(awk '{ print $3 }' \
				"/proc/$holder_pid/stat" 2>/dev/null || :)
		else
			holder_state=""
		fi
		if [ -n "$holder_pid" ] &&
		   { ! kill -0 "$holder_pid" 2>/dev/null ||
		     [ "$holder_state" = Z ]; }; then
			if wait "$holder_pid"; then
				helper_status=0
			else
				helper_status=$?
			fi
			holder_pid=""
			return 2
		fi
		sleep 0.05
	done
	return 1
}

require_helper_file()
{
	if wait_file "$1"; then
		return 0
	else
		status=$?
	fi
	if [ "$status" -eq 2 ]; then
		[ "$helper_status" -eq "$ksft_skip" ] && skip "$2 is unavailable"
		fail "retained helper exited with status $helper_status while $2"
	fi
	fail "retained helper timed out while $2"
}

written_sectors()
{
	awk '{ print $7 }' "/sys/block/${offload_backing##*/}/stat"
}

vmstat_value()
{
	awk -v name="$1" '$1 == name { print $2 }' /proc/vmstat
}

wait_offload_quiet()
{
	# A bio queued inside dm-delay is not yet visible in either the backing
	# device statistics or the mapped device's inflight counters.
	sleep 4
	previous=-1
	stable=0
	for _ in $(seq 1 240); do
		current=$(written_sectors)
		read -r reads writes < "$dm_inflight"
		if [ "$current" -eq "$previous" ] && \
		   [ "$reads" -eq 0 ] && [ "$writes" -eq 0 ]; then
			stable=$((stable + 1))
			[ "$stable" -ge 20 ] && return 0
		else
			stable=0
		fi
		previous=$current
		sleep 0.05
	done
	return 1
}

check_prereqs
[ -x ./swap_offload ] || skip "swap_offload helper is unavailable"
command -v dmsetup >/dev/null 2>&1 || skip "dmsetup is unavailable"
command -v blockdev >/dev/null 2>&1 || skip "blockdev is unavailable"
[ -e /sys/fs/cgroup/cgroup.controllers ] ||
	skip "cgroup v2 controllers are unavailable"
grep -qw memory /sys/fs/cgroup/cgroup.controllers ||
	skip "memory controller is unavailable"
[ -d /sys/kernel/mm/transparent_hugepage ] ||
	skip "transparent huge pages are unavailable"
[ -r /sys/kernel/mm/transparent_hugepage/hpage_pmd_size ] ||
	skip "PMD huge-page size is unavailable"
grep -q '^swpout_offload_refused ' /proc/vmstat ||
	skip "offload refusal counters are unavailable"

thp_size=$(cat /sys/kernel/mm/transparent_hugepage/hpage_pmd_size)
case "$thp_size" in
	''|*[!0-9]*) skip "invalid PMD huge-page size: $thp_size" ;;
esac
[ "$thp_size" -gt 0 ] || skip "PMD huge-page size is zero"
page_kib=$(awk '/KernelPageSize:/ { print $2; exit }' /proc/self/smaps)
[ "${page_kib:-0}" -gt 0 ] || skip "cannot determine the base page size"
page_size=$((page_kib * 1024))
[ "$page_size" -gt 0 ] || skip "base page size is zero"
[ $((thp_size % page_size)) -eq 0 ] ||
	skip "PMD huge-page size is not page aligned"
thp_kib=$((thp_size / 1024))
thp_sectors=$((thp_size / 512))
expected_retained_kib=$((thp_kib - page_kib))
expected_refused=$((thp_size / page_size))

mkdir "$tmp"
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
dmsetup targets > "$tmp/dm-targets" 2>/dev/null ||
	skip "cannot query device-mapper targets"
grep -q '^delay[[:space:]]' "$tmp/dm-targets" ||
	skip "device-mapper delay target is unavailable"

if [ -e /sys/module/zswap/parameters/enabled ]; then
	zswap_enabled=$(cat /sys/module/zswap/parameters/enabled)
	echo N > /sys/module/zswap/parameters/enabled ||
		skip "cannot disable zswap"
fi

# Swap priorities are global.  The ordinary zram device must be preferred to
# any pre-existing swap, while the delayed offload device remains first for
# eligible proactive reclaim.
max_prio=$(awk 'BEGIN { max = -1 } NR > 1 && $5 > max { max = $5 } END { print max }' /proc/swaps)
[ "$max_prio" -le 32765 ] ||
	skip "cannot outrank existing swap priority $max_prio"
safe_prio=$((max_prio + 1))
offload_prio=$((max_prio + 2))

dev_num=2
zram_size=$((thp_size * 4))
[ "$zram_size" -ge 67108864 ] || zram_size=67108864
zram_sizes="$zram_size $zram_size"
zram_load
zram_set_disksizes
safe="/dev/zram${dev_start}"
offload_backing="/dev/zram$((dev_start + 1))"

sectors=$(blockdev --getsz "$offload_backing") ||
	skip "cannot read offload backing size"
[ "$sectors" -gt 0 ] || skip "offload backing has zero size"
dm_table="0 $sectors delay $offload_backing 0 0 $offload_backing 0 3000"
dmsetup --noudevsync --noudevrules create "$dm_name" --table "$dm_table" ||
	skip "cannot create delayed offload device"
dm_active=1
dmsetup --noudevsync --noudevrules info --columns --noheadings \
	--separator ' ' -o major,minor "$dm_name" > "$tmp/dm-devno" ||
	skip "cannot identify delayed offload device"
read -r dm_major dm_minor < "$tmp/dm-devno"
offload="/dev/mapper/$dm_name"
if [ ! -e "$offload" ]; then
	mkdir -p /dev/mapper
	mknod "$offload" b "$dm_major" "$dm_minor" ||
		skip "cannot create delayed offload device node"
	dm_node_created=1
fi
dm_inflight="/sys/dev/block/$dm_major:$dm_minor/inflight"
[ -r "$dm_inflight" ] || skip "cannot observe delayed offload I/O"

mkswap "$safe" >/dev/null || fail "cannot initialise safe swap"
mkswap "$offload" >/dev/null || fail "cannot initialise offload swap"
swapon -p "$safe_prio" "$safe" || fail "cannot activate safe swap"
dev_makeswap=$dev_start
safe_active=1
./swap_offload activate-no-discard "$offload" "$offload_prio" ||
	fail "cannot activate offload-only swap"
offload_active=1

cgroup_enable_memory_controller "$cgroup_root" ||
	skip "cannot enable the cgroup v2 memory controller"
mkdir "$cg" || skip "cannot create test cgroup"
[ -e "$cg/memory.max" ] ||
	skip "cgroup v2 memory controller is unavailable"
echo max > "$cg/memory.swap.max"

./swap_offload retained "$thp_size" "$cg/cgroup.procs" "$ready" \
	"$touched" "$verified" &
holder_pid=$!
require_helper_file "$ready" "creating a PMD-sized anonymous huge folio"
read -r reported_pid < "$ready"
[ "$reported_pid" -eq "$holder_pid" ] ||
	fail "retained helper reported the wrong pid"
anon_huge_kib=$(awk '/AnonHugePages:/ { print $2 }' \
	"/proc/$holder_pid/smaps_rollup")
[ "${anon_huge_kib:-0}" -ge "$thp_kib" ] ||
	skip "anonymous huge folio was not created"
echo "$TCID: anonymous_huge_kib=$anon_huge_kib"

# This is the only reclaim before the retained entry is made dirty.  It puts
# the whole folio on offload-only swap so the later single-page fault leaves
# sibling swap PTEs referring to the existing slot.
echo "$thp_size swappiness=max" > "$cg/memory.reclaim" 2>/dev/null || :
wait_offload_quiet || fail "offload device did not quiesce after setup"
kill -0 "$holder_pid" || fail "retained helper died during setup reclaim"
kill -USR1 "$holder_pid" || fail "cannot request the dirty-page transition"
require_helper_file "$touched" "faulting and dirtying the retained entry"
retained_kib=$(awk '/VmSwap:/ { print $2 }' "/proc/$holder_pid/status")
[ "${retained_kib:-0}" -eq "$expected_retained_kib" ] ||
	fail "retained $retained_kib KiB, expected $expected_retained_kib KiB"
echo "$TCID: retained_kib=$retained_kib"

wait_offload_quiet || fail "offload device did not quiesce before pressure"
ordinary_before=$(written_sectors)
refused_before=$(vmstat_value swpout_offload_refused)
echo $((thp_size / 2)) > "$cg/memory.high" ||
	fail "ordinary pressure reclaim failed"
wait_offload_quiet || fail "offload device did not quiesce after pressure"
ordinary_after=$(written_sectors)
refused_after=$(vmstat_value swpout_offload_refused)
ordinary_writes=$((ordinary_after - ordinary_before))
refused_pages=$((refused_after - refused_before))
echo "$TCID: ordinary_sectors=$ordinary_writes refused_pages=$refused_pages"
[ "$ordinary_writes" -eq 0 ] ||
	fail "ordinary reclaim wrote $ordinary_writes offload sectors"
[ "$refused_pages" -ge "$expected_refused" ] ||
	fail "ordinary reclaim refused $refused_pages pages, expected at least $expected_refused"
kill -0 "$holder_pid" || fail "retained helper died after refused write"
rm -f "$verified"
kill -USR2 "$holder_pid" || fail "cannot request dirty-byte verification"
require_helper_file "$verified" "verifying the dirty retained byte"

# Proactive reclaim must still be able to rewrite the retained PMD-sized slot.
# Repeated requests make the test insensitive to a short-lived writeback
# collision; the backing-sector delta still requires exactly one folio write.
wait_offload_quiet || fail "offload device did not quiesce before recovery"
recovery_before=$(written_sectors)
echo max > "$cg/memory.high"
for _ in $(seq 1 8); do
	echo "$thp_size swappiness=max" > "$cg/memory.reclaim" 2>/dev/null || :
done
wait_offload_quiet || fail "offload device did not quiesce after recovery"
recovery_after=$(written_sectors)
recovery_writes=$((recovery_after - recovery_before))
echo "$TCID: recovery_sectors=$recovery_writes"
[ "$recovery_writes" -eq "$thp_sectors" ] ||
	fail "proactive recovery wrote $recovery_writes sectors, expected $thp_sectors"
kill -0 "$holder_pid" || fail "retained helper died during recovery"
rm -f "$verified"
kill -ALRM "$holder_pid" || fail "cannot request full data verification"
require_helper_file "$verified" "verifying all retained data"
kill -0 "$holder_pid" || fail "retained helper failed full data verification"

kill "$holder_pid" || fail "cannot stop retained helper"
if wait "$holder_pid"; then
	:
else
	status=$?
	[ "$status" -eq 143 ] || fail "retained helper exited with status $status"
fi
holder_pid=""
swapoff "$offload" || fail "cannot deactivate offload-only swap"
offload_active=0
swapoff "$safe" || fail "cannot deactivate safe swap"
safe_active=0
dev_makeswap=-1
dmsetup --noudevsync --noudevrules remove "$dm_name" ||
	fail "cannot remove delayed offload device"
dm_active=0
if [ "$dm_node_created" -eq 1 ]; then
	rm -f "$offload" || fail "cannot remove delayed offload device node"
	dm_node_created=0
fi
rmdir "$cg" || fail "cannot remove test cgroup"
cg=""
cgroup_disable_memory_controller "$cgroup_root" ||
	fail "cannot restore the cgroup memory controller"
for i in $(seq "$dev_start" "$dev_end"); do
	echo 1 > "/sys/block/zram${i}/reset" || fail "cannot reset zram$i"
	if [ "$sys_control" -eq 1 ]; then
		echo "$i" > /sys/class/zram-control/hot_remove ||
			fail "cannot remove zram$i"
	fi
done
if [ "$module_load" -eq 1 ]; then
	rmmod zram || fail "cannot unload zram"
fi
dev_end=-1
if [ -n "$zswap_enabled" ]; then
	echo "$zswap_enabled" > /sys/module/zswap/parameters/enabled ||
		fail "cannot restore zswap"
	zswap_enabled=""
fi
rm -rf "$tmp" || fail "cannot remove temporary files"
tmp=""

echo "$TCID: [PASS]"
