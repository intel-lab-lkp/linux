/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM shmem

#if !defined(_TRACE_SHMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SHMEM_H

#include <linux/types.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(mm_shmem_op_page_cache,

	TP_PROTO(struct folio *folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		__field(unsigned long, pfn)
		__field(unsigned long, i_ino)
		__field(unsigned long, index)
		__field(dev_t, s_dev)
		__field(unsigned char, order)
	),

	TP_fast_assign(
		__entry->pfn = folio_pfn(folio);
		__entry->i_ino = folio->mapping->host->i_ino;
		__entry->index = folio->index;
		if (folio->mapping->host->i_sb)
			__entry->s_dev = folio->mapping->host->i_sb->s_dev;
		else
			__entry->s_dev = folio->mapping->host->i_rdev;
		__entry->order = folio_order(folio);
	),

	TP_printk("dev %d:%d ino %lx pfn=0x%lx ofs=%lu order=%u",
		MAJOR(__entry->s_dev), MINOR(__entry->s_dev),
		__entry->i_ino,
		__entry->pfn,
		__entry->index << PAGE_SHIFT,
		__entry->order)
);

DEFINE_EVENT(mm_shmem_op_page_cache, mm_shmem_add_to_page_cache,
	TP_PROTO(struct folio *folio),
	TP_ARGS(folio)
	);

#endif /* _TRACE_SHMEM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
