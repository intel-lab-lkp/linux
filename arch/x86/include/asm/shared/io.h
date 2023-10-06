/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_IO_H
#define _ASM_X86_SHARED_IO_H

#include <linux/tracepoint-defs.h>
#include <linux/trace_portio.h>
#include <linux/types.h>

DECLARE_TRACEPOINT(portio_write);
DECLARE_TRACEPOINT(portio_read);

#define BUILDIO(bwl, bw, type)						\
static inline void __out##bwl(type value, u16 port)			\
{									\
	asm volatile("out" #bwl " %" #bw "0, %w1"			\
		     : : "a"(value), "Nd"(port));			\
	if (tracepoint_enabled(portio_write))				\
		do_trace_portio_write(value, port, #bwl[0]);		\
}									\
									\
static inline type __in##bwl(u16 port)					\
{									\
	type value;							\
	asm volatile("in" #bwl " %w1, %" #bw "0"			\
		     : "=a"(value) : "Nd"(port));			\
	if (tracepoint_enabled(portio_read))				\
		do_trace_portio_read(value, port, #bwl[0]);		\
	return value;							\
}

BUILDIO(b, b, u8)
BUILDIO(w, w, u16)
BUILDIO(l,  , u32)
#undef BUILDIO

#define inb __inb
#define inw __inw
#define inl __inl
#define outb __outb
#define outw __outw
#define outl __outl

#endif
