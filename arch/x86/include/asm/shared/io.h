/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_IO_H
#define _ASM_X86_SHARED_IO_H

#include <linux/instruction_pointer.h>
#include <linux/types.h>

#if defined(CONFIG_TRACEPOINTS) && !defined(BOOT_COMPRESSED_MISC_H) && !defined(BOOT_BOOT_H)
extern void do_trace_portio_read(u32 value, u16 port, char width, long ip_addr);
extern void do_trace_portio_write(u32 value, u16 port, char width, long ip_addr);
#else
static inline void do_trace_portio_read(u32 value, u16 port, char width, long ip_addr) {}
static inline void do_trace_portio_write(u32 value, u16 port, char width, long ip_addr) {}
#endif

#define BUILDIO(bwl, bw, type)						\
static inline void __out##bwl(type value, u16 port)			\
{									\
	asm volatile("out" #bwl " %" #bw "0, %w1"			\
		     : : "a"(value), "Nd"(port));			\
	do_trace_portio_write(value, port, #bwl[0], _THIS_IP_);		\
}									\
									\
static inline type __in##bwl(u16 port)					\
{									\
	type value;							\
	asm volatile("in" #bwl " %w1, %" #bw "0"			\
		     : "=a"(value) : "Nd"(port));			\
	do_trace_portio_read(value, port, #bwl[0], _THIS_IP_);		\
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
