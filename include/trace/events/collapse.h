/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM collapse

#if !defined(_TRACE_COLLAPSE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_COLLAPSE_H

#include  <linux/tracepoint.h>

#define SCAN_STATUS							\
	EM( SCAN_FAIL,			"failed")			\
	EM( SCAN_SUCCEED,		"succeeded")			\
	EM( SCAN_NO_PTE_TABLE,		"no_pte_table")			\
	EM( SCAN_PMD_MAPPED,		"page_pmd_mapped")		\
	EM( SCAN_EXCEED_NONE_PTE,	"exceed_none_pte")		\
	EM( SCAN_EXCEED_SWAP_PTE,	"exceed_swap_pte")		\
	EM( SCAN_EXCEED_SHARED_PTE,	"exceed_shared_pte")		\
	EM( SCAN_PTE_NON_PRESENT,	"pte_non_present")		\
	EM( SCAN_PTE_UFFD,		"pte_uffd_wp")			\
	EM( SCAN_PTE_MAPPED_HUGEPAGE,	"pte_mapped_hugepage")		\
	EM( SCAN_LACK_REFERENCED_PAGE,	"lack_referenced_page")		\
	EM( SCAN_PAGE_NULL,		"page_null")			\
	EM( SCAN_SCAN_ABORT,		"scan_aborted")			\
	EM( SCAN_PAGE_COUNT,		"not_suitable_page_count")	\
	EM( SCAN_PAGE_LRU,		"page_not_in_lru")		\
	EM( SCAN_PAGE_LOCK,		"page_locked")			\
	EM( SCAN_LOCK_DROPPED,		"lock_dropped")			\
	EM( SCAN_PAGE_ANON,		"page_not_anon")		\
	EM( SCAN_PAGE_LAZYFREE,		"page_lazyfree")		\
	EM( SCAN_PAGE_COMPOUND,		"page_compound")		\
	EM( SCAN_ANY_PROCESS,		"no_process_for_page")		\
	EM( SCAN_VMA_NULL,		"vma_null")			\
	EM( SCAN_VMA_LOCK,		"vma_not_lockable")		\
	EM( SCAN_VMA_CHECK,		"vma_check_failed")		\
	EM( SCAN_ADDRESS_RANGE,		"not_suitable_address_range")	\
	EM( SCAN_DEL_PAGE_LRU,		"could_not_delete_page_from_lru")\
	EM( SCAN_ALLOC_HUGE_PAGE_FAIL,	"alloc_huge_page_failed")	\
	EM( SCAN_CGROUP_CHARGE_FAIL,	"ccgroup_charge_failed")	\
	EM( SCAN_TRUNCATED,		"truncated")			\
	EM( SCAN_PAGE_HAS_PRIVATE,	"page_has_private")		\
	EM( SCAN_STORE_FAILED,		"store_failed")			\
	EM( SCAN_COPY_MC,		"copy_poisoned_page")		\
	EM( SCAN_PAGE_FILLED,		"page_filled")			\
	EM( SCAN_PAGE_DIRTY_OR_WRITEBACK, "page_dirty_or_writeback")	\
	EM( SCAN_PAGE_NOT_EXCLUSIVE,	"page_not_exclusive")		\
	EMe(SCAN_ALLOC_LIGHT_MISS,	"alloc_light_miss")

#define COLLAPSE_PASS_STATUS						\
	EM( COLLAPSE_PASS_ALLOC,	"alloc")			\
	EM( COLLAPSE_PASS_REVALIDATE,	"revalidate")			\
	EM( COLLAPSE_PASS_FAULTIN,	"faultin")			\
	EM( COLLAPSE_PASS_FREEZE,	"freeze")			\
	EM( COLLAPSE_PASS_COPY,		"copy")				\
	EMe(COLLAPSE_PASS_INSTALL,	"install")

#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(a);

SCAN_STATUS
COLLAPSE_PASS_STATUS

#undef EM
#undef EMe
#define EM(a, b)	{a, b},
#define EMe(a, b)	{a, b}

TRACE_EVENT(mm_collapse_scan,

	TP_PROTO(struct mm_struct *mm, unsigned long addr, int none_or_zero,
		 int unmapped, unsigned long orders, int result),

	TP_ARGS(mm, addr, none_or_zero, unmapped, orders, result),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned long, addr)
		__field(int, none_or_zero)
		__field(int, unmapped)
		__field(unsigned long, orders)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->addr = addr;
		__entry->none_or_zero = none_or_zero;
		__entry->unmapped = unmapped;
		__entry->orders = orders;
		__entry->result = result;
	),

	TP_printk("mm=%p, addr=0x%lx, none_or_zero=%d, unmapped=%d, orders=0x%lx, result=%s",
		__entry->mm,
		__entry->addr,
		__entry->none_or_zero,
		__entry->unmapped,
		__entry->orders,
		__print_symbolic(__entry->result, SCAN_STATUS))
);

TRACE_EVENT(mm_collapse_round,

	TP_PROTO(struct mm_struct *mm, unsigned int nr_candidates,
		 unsigned int nr_installed, int result, u64 freeze_to_wake_us),

	TP_ARGS(mm, nr_candidates, nr_installed, result, freeze_to_wake_us),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned int, nr_candidates)
		__field(unsigned int, nr_installed)
		__field(int, result)
		__field(u64, freeze_to_wake_us)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->nr_candidates = nr_candidates;
		__entry->nr_installed = nr_installed;
		__entry->result = result;
		__entry->freeze_to_wake_us = freeze_to_wake_us;
	),

	TP_printk("mm=%p, nr_candidates=%u, nr_installed=%u, result=%s, freeze_to_wake_us=%llu",
		__entry->mm,
		__entry->nr_candidates,
		__entry->nr_installed,
		__print_symbolic(__entry->result, SCAN_STATUS),
		__entry->freeze_to_wake_us)
);

TRACE_EVENT(mm_collapse_faultin,

	TP_PROTO(struct mm_struct *mm, unsigned int nr_faults, int result),

	TP_ARGS(mm, nr_faults, result),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned int, nr_faults)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->nr_faults = nr_faults;
		__entry->result = result;
	),

	TP_printk("mm=%p, nr_faults=%u, result=%s",
		__entry->mm,
		__entry->nr_faults,
		__print_symbolic(__entry->result, SCAN_STATUS))
);

TRACE_EVENT(mm_collapse_candidate,

	TP_PROTO(struct mm_struct *mm, unsigned long addr, unsigned int order,
		 int pass, int result),

	TP_ARGS(mm, addr, order, pass, result),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned long, addr)
		__field(unsigned int, order)
		__field(int, pass)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->addr = addr;
		__entry->order = order;
		__entry->pass = pass;
		__entry->result = result;
	),

	TP_printk("mm=%p, addr=0x%lx, order=%u, pass=%s, result=%s",
		__entry->mm,
		__entry->addr,
		__entry->order,
		__print_symbolic(__entry->pass, COLLAPSE_PASS_STATUS),
		__print_symbolic(__entry->result, SCAN_STATUS))
);

TRACE_EVENT(mm_collapse_scan_file,

	TP_PROTO(struct mm_struct *mm, struct folio *folio, struct file *file,
		 int present, int swap, int result),

	TP_ARGS(mm, folio, file, present, swap, result),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned long, pfn)
		__string(filename, file->f_path.dentry->d_iname)
		__field(int, present)
		__field(int, swap)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->pfn = folio ? folio_pfn(folio) : -1;
		__assign_str(filename);
		__entry->present = present;
		__entry->swap = swap;
		__entry->result = result;
	),

	TP_printk("mm=%p, scan_pfn=0x%lx, filename=%s, present=%d, swap=%d, result=%s",
		__entry->mm,
		__entry->pfn,
		__get_str(filename),
		__entry->present,
		__entry->swap,
		__print_symbolic(__entry->result, SCAN_STATUS))
);

TRACE_EVENT(mm_collapse_file,
	TP_PROTO(struct mm_struct *mm, struct folio *new_folio, pgoff_t index,
			unsigned long addr, bool is_shmem, struct file *file,
			int nr, int result),
	TP_ARGS(mm, new_folio, index, addr, is_shmem, file, nr, result),
	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned long, hpfn)
		__field(pgoff_t, index)
		__field(unsigned long, addr)
		__field(bool, is_shmem)
		__string(filename, file->f_path.dentry->d_iname)
		__field(int, nr)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->hpfn = new_folio ? folio_pfn(new_folio) : -1;
		__entry->index = index;
		__entry->addr = addr;
		__entry->is_shmem = is_shmem;
		__assign_str(filename);
		__entry->nr = nr;
		__entry->result = result;
	),

	TP_printk("mm=%p, hpage_pfn=0x%lx, index=%ld, addr=%lx, is_shmem=%d, filename=%s, nr=%d, result=%s",
		__entry->mm,
		__entry->hpfn,
		__entry->index,
		__entry->addr,
		__entry->is_shmem,
		__get_str(filename),
		__entry->nr,
		__print_symbolic(__entry->result, SCAN_STATUS))
);

TRACE_EVENT(mm_khugepaged_scan,

	TP_PROTO(struct mm_struct *mm, unsigned int progress,
		 bool full_scan_finished),

	TP_ARGS(mm, progress, full_scan_finished),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned int, progress)
		__field(bool, full_scan_finished)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->progress = progress;
		__entry->full_scan_finished = full_scan_finished;
	),

	TP_printk("mm=%p, progress=%u, full_scan_finished=%d",
		__entry->mm,
		__entry->progress,
		__entry->full_scan_finished)
);

#endif /* _TRACE_COLLAPSE_H */
#include <trace/define_trace.h>
