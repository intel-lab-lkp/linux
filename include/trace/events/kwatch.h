/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kwatch

#if !defined(_TRACE_KWATCH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KWATCH_H

#include <linux/tracepoint.h>
#include <linux/ptrace.h>

#define KWATCH_STACK_DEPTH 8

struct trace_seq;
const char *kwatch_trace_print_stack(struct trace_seq *p,
				     const unsigned long *stack,
				     unsigned int nr);

TRACE_EVENT(kwatch_hit,
	TP_PROTO(unsigned long ip, unsigned long sp, unsigned long addr,
		 u64 time_ns,
		 unsigned long *stack_entries, unsigned int stack_nr),
	TP_ARGS(ip, sp, addr, time_ns, stack_entries, stack_nr),

	TP_STRUCT__entry(
		__field(unsigned long, ip)
		__field(unsigned long, sp)
		__field(unsigned long, addr)
		__field(u64, time_ns)
		__field(unsigned int, stack_nr)
		__array(unsigned long, stack, KWATCH_STACK_DEPTH)
	),

	TP_fast_assign(
		unsigned int i;

		__entry->ip = ip;
		__entry->sp = sp;
		__entry->addr = addr;
		__entry->time_ns = time_ns;
		__entry->stack_nr = min_t(unsigned int, stack_nr,
					  KWATCH_STACK_DEPTH);
		for (i = 0; i < __entry->stack_nr; i++)
			__entry->stack[i] = stack_entries[i];
	),

	TP_printk("KWatch HIT: time=%llu.%06lu ip=%pS addr=0x%lx%s",
		  __entry->time_ns / 1000000000ULL,
		  (unsigned long)((__entry->time_ns / 1000ULL) % 1000000ULL),
		  (void *)__entry->ip, __entry->addr,
		  kwatch_trace_print_stack(p, __entry->stack,
					   __entry->stack_nr))
);

#endif /* _TRACE_KWATCH_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
