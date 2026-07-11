/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ref_trace

#if !defined(_TRACE_REF_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_REF_TRACE_H

#include <linux/tracepoint.h>

/**
 * ref_trace_final_put - trace when a reference count reaches zero
 * @caller: return address of refcount
 * function(refcount_sub_and_test, percpu_ref_put_many)
 * @fn: refcounting function(eg refcount_sub_and_test)
 * @obj: refcount object(struct percpu_ref, refcount_t)
 *
 * Tracepoint instrumentation can be added using the do_ref_trace_final_put
 * macro defined in include/linux/ref_trace.h
 * which uses _RET_IP_ and __func__ for caller and fn arguments respectively,
 * thus only requiring obj arg to be supplied
 */
TRACE_EVENT(ref_trace_final_put,

	TP_PROTO(unsigned long caller, const char *fn, const void *obj),

	TP_ARGS(caller, fn, obj),

	TP_STRUCT__entry(
	__field(unsigned long, caller)
	__string(fn, fn)
	__field(const void *, obj)
	),

	TP_fast_assign(
	__entry->caller = caller;
	__assign_str(fn);
	__entry->obj = obj;
	),

	TP_printk("caller=%pS fn=%s obj=%p", (void *)__entry->caller, __get_str(fn), __entry->obj)
);

#endif /* _TRACE_REF_TRACE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
