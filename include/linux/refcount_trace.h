/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REFCOUNT_TRACE_H
#define _LINUX_REFCOUNT_TRACE_H

#include <linux/tracepoint-defs.h>
#include <linux/instruction_pointer.h>

#ifdef CONFIG_REFCOUNT_TRACE_FINAL_PUT
/* Declare the tracepoint so tracepoint_enabled() can be used */
DECLARE_TRACEPOINT(refcount_final_put);

/* Wrapper function implemented in lib/ref_trace.c */
extern void do_refcount_trace_final_put(unsigned long caller, unsigned long ip, const void *obj);

#define do_trace_refcount_final_put(obj)					\
	do {									\
		if (tracepoint_enabled(refcount_final_put))			\
			do_refcount_trace_final_put(_RET_IP_, _THIS_IP_, obj);	\
	} while (0)

#define do_trace_refcount_final_put_cond(cond, obj)				\
	do {									\
		if (tracepoint_enabled(refcount_final_put) && cond)		\
			do_refcount_trace_final_put(_RET_IP_, _THIS_IP_, obj);	\
	} while (0)

#else /* !CONFIG_REFCOUNT_TRACE_FINAL_PUT */
extern void do_refcount_trace_final_put(unsigned long caller, unsigned long ip, const void *obj);
#define do_trace_refcount_final_put(obj) do { } while (0)
#define do_trace_refcount_final_put_cond(cond, obj) do { } while (0)
#endif

#endif /* _LINUX_REFCOUNT_TRACE_H */
