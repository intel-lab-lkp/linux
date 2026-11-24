#!/bin/bash
# perf kmem tests
# SPDX-License-Identifier: GPL-2.0

set -e

# Root permissions required for tracing events.
if [ "$(id -u)" != 0 ]; then
	echo "[Skip] No root permission"
	exit 2
fi

if [ ! -e /sys/kernel/tracing/events/kmem/kmalloc ] &&
   [ ! -e /sys/kernel/debug/tracing/events/kmem/kmalloc ]; then
	echo "[Skip] No kmem tracepoints"
	exit 2
fi

err=0
slabdata=$(mktemp /tmp/__perf_test_kmem.slab.XXXXX)
pagedata=$(mktemp /tmp/__perf_test_kmem.page.XXXXX)
out=$(mktemp /tmp/__perf_test_kmem.out.XXXXX)

cleanup() {
	rm -f "${slabdata}" "${slabdata}".old
	rm -f "${pagedata}" "${pagedata}".old
	rm -f "${out}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

fail() {
	echo "$1 [Failed: $2]"
	cat "${out}"
	err=1
}

test_kmem_record() {
	echo "Kmem record"
	perf kmem record -o "${slabdata}" -- sleep 1
	echo "Kmem record [Success]"
}

test_kmem_slab_stat() {
	echo "Kmem slab stat"
	if ! perf kmem -i "${slabdata}" stat > "${out}" 2>&1; then
		fail "Kmem slab stat" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (SLAB allocator)" "${out}"; then
		fail "Kmem slab stat" "missing summary"
		return
	fi
	echo "Kmem slab stat [Success]"
}

test_kmem_slab_stat_options() {
	echo "Kmem slab stat options"
	if ! perf kmem -i "${slabdata}" --sort hit,bytes --line 5 stat > "${out}" 2>&1; then
		fail "Kmem slab stat options" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (SLAB allocator)" "${out}"; then
		fail "Kmem slab stat options" "missing summary"
		return
	fi
	echo "Kmem slab stat options [Success]"
}

test_kmem_page_record() {
	echo "Kmem page record"
	perf kmem --page record -c 1000 -o "${pagedata}" -- sleep 1
	echo "Kmem page record [Success]"
}

test_kmem_page_stat() {
	echo "Kmem page stat"
	if ! perf kmem --page -i "${pagedata}" stat > "${out}" 2>&1; then
		fail "Kmem page stat" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page stat" "missing summary"
		return
	fi
	echo "Kmem page stat [Success]"
}

test_kmem_page_stat_options() {
	echo "Kmem page stat options"
	if ! perf kmem --page -i "${pagedata}" --sort order,gfp --line 5 stat > "${out}" 2>&1; then
		fail "Kmem page stat options" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page stat options" "missing summary"
		return
	fi
	echo "Kmem page stat options [Success]"
}

test_kmem_slab_alloc() {
	echo "Kmem slab alloc stats"
	if ! perf kmem -i "${slabdata}" --alloc --sort bytes --line 5 stat > "${out}" 2>&1; then
		fail "Kmem slab alloc stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (SLAB allocator)" "${out}"; then
		fail "Kmem slab alloc stats" "missing summary"
		return
	fi
	echo "Kmem slab alloc stats [Success]"
}

test_kmem_page_caller() {
	echo "Kmem page caller stats"
	if ! perf kmem --page -i "${pagedata}" --caller --sort bytes --line 5 stat > "${out}" 2>&1; then
		fail "Kmem page caller stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page caller stats" "missing summary"
		return
	fi
	echo "Kmem page caller stats [Success]"
}

test_kmem_page_caller_raw() {
	echo "Kmem page caller raw ip stats"
	if ! perf kmem --page -i "${pagedata}" --caller --raw-ip --sort callsite --line 5 stat > "${out}" 2>&1; then
		fail "Kmem page caller raw ip stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page caller raw ip stats" "missing summary"
		return
	fi
	echo "Kmem page caller raw ip stats [Success]"
}

test_kmem_page_alloc() {
	echo "Kmem page alloc stats"
	if ! perf kmem --page -i "${pagedata}" --alloc --sort bytes --line 5 stat > "${out}" 2>&1; then
		fail "Kmem page alloc stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page alloc stats" "missing summary"
		return
	fi
	echo "Kmem page alloc stats [Success]"
}

test_kmem_page_live() {
	echo "Kmem page live stats"
	if ! perf kmem --page --live -i "${pagedata}" --sort page --line 5 stat > "${out}" 2>&1; then
		fail "Kmem page live stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (page allocator)" "${out}"; then
		fail "Kmem page live stats" "missing summary"
		return
	fi
	echo "Kmem page live stats [Success]"
}

test_kmem_slab_caller() {
	echo "Kmem slab caller stats"
	if ! perf kmem -i "${slabdata}" --caller --sort callsite --line 5 stat > "${out}" 2>&1; then
		fail "Kmem slab caller stats" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (SLAB allocator)" "${out}"; then
		fail "Kmem slab caller stats" "missing summary"
		return
	fi
	echo "Kmem slab caller stats [Success]"
}

test_kmem_time_filter() {
	echo "Kmem time filter"
	if ! perf kmem -i "${slabdata}" --time 0, stat > "${out}" 2>&1; then
		fail "Kmem time filter" "perf kmem stat failed"
		return
	fi
	if ! grep -q "SUMMARY (SLAB allocator)" "${out}"; then
		fail "Kmem time filter" "missing summary"
		return
	fi
	echo "Kmem time filter [Success]"
}

test_kmem_record
test_kmem_slab_stat
test_kmem_slab_stat_options
test_kmem_slab_alloc
test_kmem_slab_caller
test_kmem_time_filter
test_kmem_page_record
test_kmem_page_stat
test_kmem_page_stat_options
test_kmem_page_caller
test_kmem_page_caller_raw
test_kmem_page_alloc
test_kmem_page_live

cleanup
exit $err
