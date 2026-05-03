/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rmap

#if !defined(_TRACE_RMAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RMAP_H

#include <linux/tracepoint.h>
#include <linux/rmap.h>

#define GET_RMAP_PAGE_TYPE(folio) (folio_test_ksm(folio) ? "ksm" : \
		(folio_test_anon(folio) ? "anon" : "file"))

TRACE_EVENT(rmap_walk,

	TP_PROTO(struct folio *folio, struct rmap_walk_control *rwc, u64 duration_ns, bool locked),

	TP_ARGS(folio, rwc, duration_ns, locked),

	TP_STRUCT__entry(
		__field(unsigned long, folio_addr)
		__field(unsigned long, rwc_addr)
		__field(u64, duration_ns)
		__string(page_type, GET_RMAP_PAGE_TYPE(folio))
		__field(bool, locked)
	),

	TP_fast_assign(
		__entry->folio_addr = (unsigned long)folio;
		__entry->rwc_addr = (unsigned long)rwc;
		__entry->duration_ns = duration_ns;
		__assign_str(page_type);
		__entry->locked = locked;
	),

	TP_printk("folio=%p rwc=%p duration_ns=%llu page_type=%s locked=%s",
		(void *)(unsigned long)__entry->folio_addr,
		(void *)(unsigned long)__entry->rwc_addr,
		__entry->duration_ns,
		__get_str(page_type),
		__entry->locked ? "true" : "false")
);



#endif /* _TRACE_RMAP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
