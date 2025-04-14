/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM exceptions

#if !defined(_TRACE_PAGE_FAULT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PAGE_FAULT_H

#include <linux/tracepoint.h>
#include <asm/trace/common.h>

extern int trace_pagefault_enter_reg(void);
extern void trace_pagefault_enter_unreg(void);
extern int trace_pagefault_exit_reg(void);
extern void trace_pagefault_exit_unreg(void);

DECLARE_EVENT_CLASS(x86_exceptions,

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
		__entry->ip = regs->ip;
		__entry->error_code = error_code;
	),

	TP_printk("address=%ps ip=%ps error_code=0x%lx",
		  (void *)__entry->address, (void *)__entry->ip,
		  __entry->error_code) );

#define DEFINE_PAGE_FAULT_EVENT(name, reg, unreg)		\
DEFINE_EVENT_FN(x86_exceptions, name,				\
	TP_PROTO(unsigned long address,	struct pt_regs *regs,	\
		 unsigned long error_code),			\
	TP_ARGS(address, regs, error_code),			\
	reg, unreg)

DEFINE_PAGE_FAULT_EVENT(
	page_fault_user_enter,
	trace_pagefault_enter_reg,
	trace_pagefault_enter_unreg
);
DEFINE_PAGE_FAULT_EVENT(
	page_fault_user_exit,
	trace_pagefault_exit_reg,
	trace_pagefault_exit_unreg
);
DEFINE_PAGE_FAULT_EVENT(
	page_fault_kernel_enter,
	trace_pagefault_enter_reg,
	trace_pagefault_enter_unreg
);
DEFINE_PAGE_FAULT_EVENT(
	page_fault_kernel_exit,
	trace_pagefault_exit_reg,
	trace_pagefault_exit_unreg
);

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE exceptions
#endif /*  _TRACE_PAGE_FAULT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
