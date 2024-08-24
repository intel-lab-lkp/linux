/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Based on arch/x86/include/asm/trace/exceptions.h
 *
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM exceptions

#if !defined(_TRACE_MEM_ABORT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MEM_ABORT_H

#include <linux/tracepoint.h>
#include <asm/trace/common.h>

extern int trace_memabort_reg(void);
extern void trace_memabort_unreg(void);

DECLARE_EVENT_CLASS(arm64_exceptions,

	TP_PROTO(unsigned long address, struct pt_regs *regs,
		 unsigned long error_code),

	TP_ARGS(address, regs, error_code),

	TP_STRUCT__entry(
		__field(		unsigned long, address	)
		__field(		unsigned long, ip	)
		__field(		unsigned long, error_code )
	),

	TP_fast_assign(
		__entry->address = address;
		__entry->ip = regs->pc;
		__entry->error_code = error_code;
	),

	TP_printk("address=%ps ip=%ps error_code=0x%lx",
		  (void *)__entry->address, (void *)__entry->ip,
		  __entry->error_code) );

#define DEFINE_MEM_ABORT_EVENT(name)				\
DEFINE_EVENT_FN(arm64_exceptions, name,				\
	TP_PROTO(unsigned long address,	struct pt_regs *regs,	\
		 unsigned long error_code),			\
	TP_ARGS(address, regs, error_code),			\
	trace_memabort_reg, trace_memabort_unreg);

DEFINE_MEM_ABORT_EVENT(mem_abort_user);
DEFINE_MEM_ABORT_EVENT(mem_abort_kernel);

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH asm/trace
#define TRACE_INCLUDE_FILE exceptions
#endif /*  _TRACE_MEM_ABORT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
