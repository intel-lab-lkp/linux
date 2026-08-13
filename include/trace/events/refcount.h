/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM refcount

#if !defined(_TRACE_REFCOUNT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_REFCOUNT_H

#include <linux/tracepoint.h>

#ifdef CONFIG_REFCOUNT_TRACE_FINAL_PUT

/**
 * refcount_final_put - trace when a reference count reaches zero
 * @caller: return address of refcount
 * function(refcount_sub_and_test, percpu_ref_put_many)
 * @ip: return address of trace wrapper macro call,
 * inlining may cause it to be something else tho
 * @obj: refcount object(struct percpu_ref, refcount_t)
 *
 * Tracepoint instrumentation can be added using the do_refcount_trace_final_put
 * macro defined in include/linux/refcount_trace.h
 * which uses _RET_IP_ and _THIS_IP_ for caller and ip arguments respectively,
 * thus only requiring obj arg to be supplied
 */
TRACE_EVENT(refcount_final_put,

	TP_PROTO(unsigned long caller, unsigned long ip, const void *obj),

	TP_ARGS(caller, ip, obj),

	TP_STRUCT__entry(
		__field(	unsigned long,		caller	)
		__field(	unsigned long,		ip	)
		__field(	const void *,		obj	)
	),

	TP_fast_assign(
		__entry->caller = caller;
		__entry->ip = ip;
		__entry->obj = obj;
	),

	TP_printk("caller=%pS ip=%pS obj=%p",
		  (void *)__entry->caller,
		  (void *)__entry->ip,
		  __entry->obj
	)
);

#endif /* CONFIG_REFCOUNT_TRACE_FINAL_PUT */

#endif /* _TRACE_REFCOUNT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
