/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kwatch

#if !defined(_TRACE_KWATCH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KWATCH_H

#include <linux/tracepoint.h>
#include <linux/ptrace.h>
#include <linux/math64.h>

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
		/*
		 * time_ns first: u64 leading the entry avoids a 4-byte hole
		 * after the unsigned-long fields on 32-bit. stack_nr trails
		 * the fixed fields for the same reason; the stack is a
		 * dynamic array sized to what was actually captured, so a
		 * short trace neither wastes space nor leaks uninitialized
		 * tail slots.
		 */
		__field(u64, time_ns)
		__field(unsigned long, ip)
		__field(unsigned long, sp)
		__field(unsigned long, addr)
		__dynamic_array(unsigned long, stack,
				min_t(unsigned int, stack_nr, KWATCH_STACK_DEPTH))
		__field(unsigned int, stack_nr)
	),

	TP_fast_assign(
		unsigned long *stack = __get_dynamic_array(stack);
		unsigned int i;

		__entry->time_ns = time_ns;
		__entry->ip = ip;
		__entry->sp = sp;
		__entry->addr = addr;
		__entry->stack_nr = min_t(unsigned int, stack_nr,
					  KWATCH_STACK_DEPTH);
		for (i = 0; i < __entry->stack_nr; i++)
			stack[i] = stack_entries[i];
	),

	TP_printk("KWatch HIT: time=%llu.%06u ip=%pS addr=0x%lx%s",
		  div_u64(__entry->time_ns, 1000000000ULL),
		  (unsigned int)(div_u64(__entry->time_ns, 1000ULL) % 1000000ULL),
		  (void *)__entry->ip, __entry->addr,
		  kwatch_trace_print_stack(p, __get_dynamic_array(stack),
					   __entry->stack_nr))
);

#endif /* _TRACE_KWATCH_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
