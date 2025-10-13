/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM vma

#if !defined(_TRACE_VMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_VMA_H

#include <linux/tracepoint.h>

TRACE_EVENT(mm_insufficient_vma_slots,

	TP_PROTO(struct mm_struct *mm),

	TP_ARGS(mm),

	TP_STRUCT__entry(
		__field(void *,	mm)
		__field(int,	vma_count)
	),

	TP_fast_assign(
		__entry->mm		= mm;
		__entry->vma_count	= mm->vma_count;
	),

	TP_printk("mm=%p vma_count=%d", __entry->mm, __entry->vma_count)
);

#endif /*  _TRACE_VMA_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
