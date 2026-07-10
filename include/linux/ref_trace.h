/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REF_TRACE_H
#define _LINUX_REF_TRACE_H

#include <linux/tracepoint-defs.h>
#include <linux/instruction_pointer.h>

/* Declare the tracepoint so tracepoint_enabled() can be used */
DECLARE_TRACEPOINT(ref_trace_final_put);

#ifdef CONFIG_TRACEPOINTS
/* Wrapper function implemented in lib/ref_trace.c */
extern void do_ref_trace_final_put(unsigned long caller, const char *fn, const void *obj);

#define do_trace_ref_final_put(obj)						\
	do {									\
		if (tracepoint_enabled(ref_trace_final_put))			\
			do_ref_trace_final_put(_RET_IP_, __func__, obj);	\
	} while (0)

#else /* !CONFIG_TRACEPOINTS */
static inline void do_ref_trace_final_put(unsigned long caller, const char *fn, const void *obj) { }
#define do_trace_ref_final_put(obj) do { } while (0)
#endif

#endif /* _LINUX_REF_TRACE_H */
