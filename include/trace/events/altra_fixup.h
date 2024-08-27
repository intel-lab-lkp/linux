/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM altra_fixup

#if !defined(_ALTERA_FIXUP_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _ALTRA_FIXUP_H_

#include <linux/tracepoint.h>
#include <linux/io.h>

#ifdef CONFIG_ALTRA_ERRATUM_82288

TRACE_EVENT(altra_fixup_alignment,
	    TP_PROTO(unsigned long far, unsigned long esr),
	    TP_ARGS(far, esr),
	    TP_STRUCT__entry(
		__field(unsigned long, far)
		__field(unsigned long, esr)
	    ),
	    TP_fast_assign(
		__entry->far = far;
		__entry->esr = esr;
	    ),
	    TP_printk("far=0x%016lx esr=0x%016lx",
		      __entry->far, __entry->esr)
);

TRACE_EVENT(altra_mkspecial,
	    TP_PROTO(pte_t pte),
	    TP_ARGS(pte),
	    TP_STRUCT__entry(
		__field(pteval_t, pte)
	    ),
	    TP_fast_assign(
		__entry->pte = pte_val(pte);
	    ),
	    TP_printk("pte=0x%016llx", __entry->pte)
);

TRACE_EVENT(altra_ioremap_prot,
	    TP_PROTO(pgprot_t prot),
	    TP_ARGS(prot),
	    TP_STRUCT__entry(
		__field(pteval_t, pte)
	    ),
	    TP_fast_assign(
		__entry->pte = pgprot_val(prot);
	    ),
	    TP_printk("prot=0x%016llx", __entry->pte)
);

#endif /* CONFIG_ALTRA_ERRATUM_82288 */

#endif /* _ALTRA_FIXUP_H_ */

/* This part must be outside protection */
#include <trace/define_trace.h>
