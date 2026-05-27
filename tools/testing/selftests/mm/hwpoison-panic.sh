#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify vm.panic_on_unrecoverable_memory_failure by injecting a hwpoison
# error on a kernel-owned page and confirming the kernel panics.
#
# Three "kinds" of kernel-owned page can be targeted, selectable via the
# first positional argument (default: rodata):
#
#   rodata  - a PG_reserved page in the kernel rodata range
#             (sourced from /proc/iomem "Kernel rodata").  Exercises
#             memory_failure() -> get_any_page() on a PageReserved page.
#
#   slab    - a slab page found via /proc/kpageflags (KPF_SLAB).
#             Exercises memory_failure() -> get_any_page() on a non
#             PG_reserved kernel-owned page.  This path is what catches
#             regressions where get_any_page() collapses kernel-owned
#             pages into a transient -EIO instead of -ENOTRECOVERABLE.
#
#   pgtable - a page-table page found via /proc/kpageflags (KPF_PGTABLE).
#             Same path as slab, different page type.
#
# This test is DESTRUCTIVE: a successful run crashes the kernel.  It is
# meant to be executed inside a disposable VM (e.g. virtme-ng) with a
# serial console captured by the harness.  It is skipped unless the
# caller opts in via RUN_DESTRUCTIVE=1.
#
# Test passes externally: the kernel must panic with
#   "Memory failure: <pfn>: unrecoverable page"
# A return from the inject means the panic did not fire and the test
# fails.
#
# Author: Breno Leitao <leitao@debian.org>

set -u

ksft_skip=4
sysctl_path=/proc/sys/vm/panic_on_unrecoverable_memory_failure
inject_path=/sys/devices/system/memory/hard_offline_page
kpageflags_path=/proc/kpageflags

# /proc/kpageflags bit positions (see include/uapi/linux/kernel-page-flags.h)
KPF_SLAB=7
KPF_COMPOUND_TAIL=16
KPF_HWPOISON=19
KPF_NOPAGE=20
KPF_PGTABLE=26

kind=${1:-rodata}

ksft_print() { echo "# $*"; }
ksft_exit_skip() { ksft_print "$*"; exit "$ksft_skip"; }
ksft_exit_fail() { echo "not ok 1 $*"; exit 1; }

if [ "$(id -u)" -ne 0 ]; then
	ksft_exit_skip "must run as root"
fi

if [ ! -w "$sysctl_path" ]; then
	ksft_exit_skip "$sysctl_path not present (kernel without the sysctl?)"
fi

if [ ! -w "$inject_path" ]; then
	ksft_exit_skip "$inject_path not present (no MEMORY_HOTPLUG?)"
fi

if [ "${RUN_DESTRUCTIVE:-0}" != "1" ]; then
	ksft_exit_skip "destructive test; re-run with RUN_DESTRUCTIVE=1 inside a disposable VM"
fi

# Pick a PFN inside the kernel image rodata region of /proc/iomem.
# This is preferred over a top-level "Reserved" entry because top-level
# Reserved ranges are often firmware holes that have no backing struct
# page; pfn_to_online_page() returns NULL on those and memory_failure()
# bails out with -ENXIO before reaching the panic path.
#
# "Kernel rodata" is reported as a sub-resource of "System RAM" on every
# major architecture, which guarantees:
#   - the PFN is backed by struct page (within an online memory range);
#   - PG_reserved is set on the page (kernel image area);
#   - the memory is read-only, so setting PG_hwpoison on it does not
#     corrupt writable kernel state if the panic somehow does not fire.
#
# /proc/iomem entries look like (indented for sub-resources):
#     "  02500000-02ffffff : Kernel rodata"
pick_rodata_phys_addr() {
	awk -v pagesize="$(getconf PAGE_SIZE)" '
	/: Kernel rodata[[:space:]]*$/ {
		sub(/^[[:space:]]+/, "")
		n = split($0, a, /[- ]/)
		start = strtonum("0x" a[1])
		end   = strtonum("0x" a[2])
		if (end <= start)
			next
		# Page-align upward and emit the first byte of that page.
		pfn = int((start + pagesize - 1) / pagesize)
		printf "0x%x\n", pfn * pagesize
		exit 0
	}
	' /proc/iomem
}

# Walk /proc/kpageflags and return the phys addr of the first PFN that
# has bit $1 set, with KPF_HWPOISON, KPF_NOPAGE and KPF_COMPOUND_TAIL
# all clear (so we attack a real, non-tail, not-already-poisoned page).
#
# We skip the first 16 MiB of PFNs to step past low-memory special
# ranges (BIOS/EFI/ACPI/etc.) that often are PG_reserved and would not
# exhibit the slab/pgtable type we are looking for.
pick_kpageflags_phys_addr() {
	local want_bit=$1
	local pagesize skip_pfn

	[ -r "$kpageflags_path" ] || return

	pagesize=$(getconf PAGE_SIZE)
	skip_pfn=$(((16 * 1024 * 1024) / pagesize))

	od -An -tx8 -v -w8 -j "$((skip_pfn * 8))" "$kpageflags_path" 2>/dev/null | \
	awk -v want_bit="$want_bit" \
	    -v hwp_bit="$KPF_HWPOISON" \
	    -v nopage_bit="$KPF_NOPAGE" \
	    -v tail_bit="$KPF_COMPOUND_TAIL" \
	    -v base_pfn="$skip_pfn" \
	    -v pagesize="$pagesize" '
	# Test whether bit "b" is set in the 16-hex-digit value "hex".
	# Done with substring + per-digit lookup so we never rely on awk
	# bitwise operators (mawk lacks them) or 64-bit FP precision.
	function bit_set(hex, b,    di, bi, c, v) {
		di = int(b / 4)
		bi = b - di * 4
		c = substr(hex, length(hex) - di, 1)
		v = strtonum("0x" c)
		if (bi == 0) return (v % 2) == 1
		if (bi == 1) return int(v / 2) % 2 == 1
		if (bi == 2) return int(v / 4) % 2 == 1
		return int(v / 8) % 2 == 1
	}
	{
		gsub(/^[[:space:]]+/, "")
		h = $1
		if (bit_set(h, want_bit) &&
		    !bit_set(h, hwp_bit) &&
		    !bit_set(h, nopage_bit) &&
		    !bit_set(h, tail_bit)) {
			pfn = base_pfn + NR - 1
			printf "0x%x\n", pfn * pagesize
			exit 0
		}
	}
	'
}

case "$kind" in
rodata)
	phys_addr=$(pick_rodata_phys_addr)
	missing_msg='no "Kernel rodata" entry in /proc/iomem'
	;;
slab)
	phys_addr=$(pick_kpageflags_phys_addr "$KPF_SLAB")
	missing_msg="no usable slab PFN found in $kpageflags_path"
	;;
pgtable)
	phys_addr=$(pick_kpageflags_phys_addr "$KPF_PGTABLE")
	missing_msg="no usable page-table PFN found in $kpageflags_path"
	;;
*)
	ksft_exit_fail "unknown kind '$kind' (expected: rodata|slab|pgtable)"
	;;
esac

if [ -z "$phys_addr" ]; then
	ksft_exit_skip "$missing_msg"
fi

ksft_print "enabling $sysctl_path"
prior=$(cat "$sysctl_path")
echo 1 > "$sysctl_path" || ksft_exit_fail "failed to enable sysctl"

ksft_print "injecting hwpoison at phys 0x$(printf '%x' "$phys_addr") (kind=$kind)"
ksft_print "expecting kernel panic: 'Memory failure: <pfn>: unrecoverable page'"

# If this returns, the kernel did not panic → test failed.  Restore the
# sysctl before reporting so the system is left as we found it.
if echo "$phys_addr" > "$inject_path"; then
	echo "$prior" > "$sysctl_path"
	ksft_exit_fail "inject returned without panic; sysctl ineffective"
fi

# Write failed (e.g. -EINVAL on offlining a non-online region): also a
# failure for this test, since we expected the panic path.
echo "$prior" > "$sysctl_path"
ksft_exit_fail "inject failed before reaching the panic path"
