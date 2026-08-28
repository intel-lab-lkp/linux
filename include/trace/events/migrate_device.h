/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM migrate_device

#if !defined(_TRACE_MIGRATE_DEVICE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MIGRATE_DEVICE_H

#include <linux/migrate.h>
#include <linux/tracepoint.h>

/*
 * Define enums for tracing information.
 */
#ifndef __MIGRATE_DEVICE_DECLARE_TRACE_ENUMS_ONCE_ONLY
#define __MIGRATE_DEVICE_DECLARE_TRACE_ENUMS_ONCE_ONLY

/*
 * Why a page table walk callback classified the range the way it did.
 * Several call sites collect the same kind of range, so the reason is
 * what distinguishes them in a trace.
 */
enum migrate_vma_walk_reason {
	MIGRATE_VMA_WALK_HOLE,
	MIGRATE_VMA_WALK_HOLE_COMPOUND,
	MIGRATE_VMA_WALK_HUGE_PMD,
	MIGRATE_VMA_WALK_SKIP_NOT_ANON,
	MIGRATE_VMA_WALK_SKIP_NOT_SELECTED,
	MIGRATE_VMA_WALK_SKIP_OWNER_MISMATCH,
	MIGRATE_VMA_WALK_SKIP_LOCK_CONTENDED,
	MIGRATE_VMA_WALK_SKIP_COMPOUND_TAIL,
	MIGRATE_VMA_WALK_SKIP_SPLIT_FAILED,
};

#endif /* __MIGRATE_DEVICE_DECLARE_TRACE_ENUMS_ONCE_ONLY */

TRACE_DEFINE_ENUM(MIGRATE_VMA_SELECT_SYSTEM);
TRACE_DEFINE_ENUM(MIGRATE_VMA_SELECT_DEVICE_PRIVATE);
TRACE_DEFINE_ENUM(MIGRATE_VMA_SELECT_DEVICE_COHERENT);
TRACE_DEFINE_ENUM(MIGRATE_VMA_SELECT_COMPOUND);

TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_HOLE);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_HOLE_COMPOUND);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_HUGE_PMD);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_NOT_ANON);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_NOT_SELECTED);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_OWNER_MISMATCH);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_LOCK_CONTENDED);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_COMPOUND_TAIL);
TRACE_DEFINE_ENUM(MIGRATE_VMA_WALK_SKIP_SPLIT_FAILED);

#define show_walk_reason(reason)					\
	__print_symbolic(reason,					\
		{ MIGRATE_VMA_WALK_HOLE,		"HOLE" },	\
		{ MIGRATE_VMA_WALK_HOLE_COMPOUND,	"HOLE_COMPOUND" }, \
		{ MIGRATE_VMA_WALK_HUGE_PMD,		"HUGE_PMD" },	\
		{ MIGRATE_VMA_WALK_SKIP_NOT_ANON,	"NOT_ANON" },	\
		{ MIGRATE_VMA_WALK_SKIP_NOT_SELECTED,	"NOT_SELECTED" }, \
		{ MIGRATE_VMA_WALK_SKIP_OWNER_MISMATCH,	"OWNER_MISMATCH" }, \
		{ MIGRATE_VMA_WALK_SKIP_LOCK_CONTENDED,	"LOCK_CONTENDED" }, \
		{ MIGRATE_VMA_WALK_SKIP_COMPOUND_TAIL,	"COMPOUND_TAIL" }, \
		{ MIGRATE_VMA_WALK_SKIP_SPLIT_FAILED,	"SPLIT_FAILED" })

#define show_migrate_vma_flags(flags)					\
	__print_flags(flags, "|",					\
		{ MIGRATE_VMA_SELECT_SYSTEM,		"SYSTEM" },	\
		{ MIGRATE_VMA_SELECT_DEVICE_PRIVATE,	"DEVICE_PRIVATE" }, \
		{ MIGRATE_VMA_SELECT_DEVICE_COHERENT,	"DEVICE_COHERENT" }, \
		{ MIGRATE_VMA_SELECT_COMPOUND,		"COMPOUND" })

DECLARE_EVENT_CLASS(migrate_vma_range_class,

	TP_PROTO(const struct migrate_vma *migrate),

	TP_ARGS(migrate),

	TP_STRUCT__entry(
		__field(void *, vma)
		__field(void *, mm)
		__field(void *, src)
		__field(void *, dst)
		__field(void *, pgmap_owner)
		__field(void *, fault_page)
		__field(unsigned long, start)
		__field(unsigned long, end)
		__field(unsigned long, npages)
		__field(unsigned long, cpages)
		__field(unsigned long, flags)
	),

	TP_fast_assign(
		__entry->vma = migrate->vma;
		__entry->mm = migrate->vma ? migrate->vma->vm_mm : NULL;
		__entry->src = migrate->src;
		__entry->dst = migrate->dst;
		__entry->pgmap_owner = migrate->pgmap_owner;
		__entry->fault_page = migrate->fault_page;
		__entry->start = migrate->start;
		__entry->end = migrate->end;
		__entry->npages = migrate->npages;
		__entry->cpages = migrate->cpages;
		__entry->flags = migrate->flags;
	),

	TP_printk("mm=%p vma=%p range=%#lx-%#lx bytes=%lu npages=%lu cpages=%lu flags=%s src=%p dst=%p owner=%p fault_page=%p",
		__entry->mm, __entry->vma, __entry->start, __entry->end,
		__entry->end - __entry->start, __entry->npages,
		__entry->cpages,
		show_migrate_vma_flags(__entry->flags),
		__entry->src, __entry->dst, __entry->pgmap_owner,
		__entry->fault_page)
);

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_setup_start,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_setup_done,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_collect_start,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_collect_done,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_pages_start,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_pages_done,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_finalize_start,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

DEFINE_EVENT(migrate_vma_range_class, migrate_vma_finalize_done,
	TP_PROTO(const struct migrate_vma *migrate),
	TP_ARGS(migrate));

/*
 * Page table walk callbacks act on a sub-range of the migration and can be
 * reached from several call sites, so they report the range they were handed
 * and why, in addition to the migrate-wide state.
 */
DECLARE_EVENT_CLASS(migrate_vma_walk_class,

	TP_PROTO(const struct migrate_vma *migrate, unsigned long start,
		 unsigned long end, enum migrate_vma_walk_reason reason),

	TP_ARGS(migrate, start, end, reason),

	TP_STRUCT__entry(
		__field(void *, vma)
		__field(void *, mm)
		__field(void *, src)
		__field(void *, dst)
		__field(void *, pgmap_owner)
		__field(void *, fault_page)
		__field(unsigned long, walk_start)
		__field(unsigned long, walk_end)
		__field(unsigned long, start)
		__field(unsigned long, end)
		__field(unsigned long, npages)
		__field(unsigned long, cpages)
		__field(unsigned long, flags)
		__field(unsigned int, reason)
	),

	TP_fast_assign(
		__entry->vma = migrate->vma;
		__entry->mm = migrate->vma ? migrate->vma->vm_mm : NULL;
		__entry->src = migrate->src;
		__entry->dst = migrate->dst;
		__entry->pgmap_owner = migrate->pgmap_owner;
		__entry->fault_page = migrate->fault_page;
		__entry->walk_start = start;
		__entry->walk_end = end;
		__entry->start = migrate->start;
		__entry->end = migrate->end;
		__entry->npages = migrate->npages;
		__entry->cpages = migrate->cpages;
		__entry->flags = migrate->flags;
		__entry->reason = reason;
	),

	TP_printk("mm=%p vma=%p walk=%#lx-%#lx bytes=%lu reason=%s range=%#lx-%#lx npages=%lu cpages=%lu flags=%s src=%p dst=%p owner=%p fault_page=%p",
		__entry->mm, __entry->vma,
		__entry->walk_start, __entry->walk_end,
		__entry->walk_end - __entry->walk_start,
		show_walk_reason(__entry->reason),
		__entry->start, __entry->end, __entry->npages,
		__entry->cpages,
		show_migrate_vma_flags(__entry->flags),
		__entry->src, __entry->dst, __entry->pgmap_owner,
		__entry->fault_page)
);

DEFINE_EVENT(migrate_vma_walk_class, migrate_vma_collect_skip,
	TP_PROTO(const struct migrate_vma *migrate, unsigned long start,
		 unsigned long end, enum migrate_vma_walk_reason reason),
	TP_ARGS(migrate, start, end, reason));

DEFINE_EVENT(migrate_vma_walk_class, migrate_vma_collect_hole,
	TP_PROTO(const struct migrate_vma *migrate, unsigned long start,
		 unsigned long end, enum migrate_vma_walk_reason reason),
	TP_ARGS(migrate, start, end, reason));

DEFINE_EVENT(migrate_vma_walk_class, migrate_vma_collect_huge_pmd,
	TP_PROTO(const struct migrate_vma *migrate, unsigned long start,
		 unsigned long end, enum migrate_vma_walk_reason reason),
	TP_ARGS(migrate, start, end, reason));

DECLARE_EVENT_CLASS(migrate_device_batch_class,

	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),

	TP_ARGS(src, dst, npages),

	TP_STRUCT__entry(
		__field(void *, src)
		__field(void *, dst)
		__field(unsigned long, src_head)
		__field(unsigned long, dst_head)
		__field(unsigned long, npages)
	),

	TP_fast_assign(
		__entry->src = (void *)src;
		__entry->dst = (void *)dst;
		__entry->src_head = npages && src ? src[0] : 0;
		__entry->dst_head = npages && dst ? dst[0] : 0;
		__entry->npages = npages;
	),

	TP_printk("src=%p dst=%p npages=%lu src_head=%#lx dst_head=%#lx",
		__entry->src, __entry->dst, __entry->npages,
		__entry->src_head, __entry->dst_head)
);

DEFINE_EVENT(migrate_device_batch_class, migrate_device_unmap_done,
	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),
	TP_ARGS(src, dst, npages));

DEFINE_EVENT(migrate_device_batch_class, migrate_device_pages_start,
	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),
	TP_ARGS(src, dst, npages));

DEFINE_EVENT(migrate_device_batch_class, migrate_device_pages_done,
	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),
	TP_ARGS(src, dst, npages));

DEFINE_EVENT(migrate_device_batch_class, migrate_device_finalize_start,
	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),
	TP_ARGS(src, dst, npages));

DEFINE_EVENT(migrate_device_batch_class, migrate_device_finalize_done,
	TP_PROTO(const unsigned long *src, const unsigned long *dst,
		 unsigned long npages),
	TP_ARGS(src, dst, npages));

DECLARE_EVENT_CLASS(migrate_device_folio_class,

	TP_PROTO(unsigned long index, unsigned long nr_pages,
		 unsigned long src, unsigned long dst),

	TP_ARGS(index, nr_pages, src, dst),

	TP_STRUCT__entry(
		__field(unsigned long, index)
		__field(unsigned long, nr_pages)
		__field(unsigned long, src)
		__field(unsigned long, dst)
		__field(unsigned long, src_pfn)
		__field(unsigned long, dst_pfn)
	),

	TP_fast_assign(
		__entry->index = index;
		__entry->nr_pages = nr_pages;
		__entry->src = src;
		__entry->dst = dst;
		__entry->src_pfn = src >> MIGRATE_PFN_SHIFT;
		__entry->dst_pfn = dst >> MIGRATE_PFN_SHIFT;
	),

	TP_printk("index=%lu nr_pages=%lu src=%#lx src_pfn=%#lx src_flags=%s dst=%#lx dst_pfn=%#lx dst_flags=%s",
		__entry->index, __entry->nr_pages, __entry->src,
		__entry->src_pfn,
		__print_flags(__entry->src & ((1UL << MIGRATE_PFN_SHIFT) - 1),
			"|",
			{ MIGRATE_PFN_VALID, "VALID" },
			{ MIGRATE_PFN_MIGRATE, "MIGRATE" },
			{ MIGRATE_PFN_WRITE, "WRITE" },
			{ MIGRATE_PFN_COMPOUND, "COMPOUND" }),
		__entry->dst, __entry->dst_pfn,
		__print_flags(__entry->dst & ((1UL << MIGRATE_PFN_SHIFT) - 1),
			"|",
			{ MIGRATE_PFN_VALID, "VALID" },
			{ MIGRATE_PFN_MIGRATE, "MIGRATE" },
			{ MIGRATE_PFN_WRITE, "WRITE" },
			{ MIGRATE_PFN_COMPOUND, "COMPOUND" }))
);

DEFINE_EVENT(migrate_device_folio_class, migrate_device_folio_finalize,
	TP_PROTO(unsigned long index, unsigned long nr_pages,
		 unsigned long src, unsigned long dst),
	TP_ARGS(index, nr_pages, src, dst));

#endif /* _TRACE_MIGRATE_DEVICE_H */

#include <trace/define_trace.h>
