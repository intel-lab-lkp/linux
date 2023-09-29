/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_IO_H
#define _ASM_X86_SHARED_IO_H

#include <linux/trace_portio.h>
#include <linux/types.h>

/*
 * We don't want the tracing logic included in the early boot modules (under
 * arch/x86/boot) so we check for their include guards here.  If we don't do
 * this we will get compiler errors.  These checks are not present in
 * arch/x86/include/asm/msr.h which contains similar tracing logic.  That is
 * possible only because none of the msr inline functions are instantiated in
 * the early boot modules.  If that changes this issue will need to be addressed
 * there as well.  Therefore it might be better to handle this centrally in
 * tracepoint-defs.h.
 */

#if defined(CONFIG_TRACEPOINTS) && !defined(BOOT_COMPRESSED_MISC_H) && !defined(BOOT_BOOT_H)
#include <linux/tracepoint-defs.h>
DECLARE_TRACEPOINT(portio_write);
DECLARE_TRACEPOINT(portio_read);
#define _tracepoint_enabled(tracepoint) tracepoint_enabled(tracepoint)
#else
#define _tracepoint_enabled(tracepoint) false
#endif

#define BUILDIO(bwl, bw, type)						\
static inline void __out##bwl(type value, u16 port)			\
{									\
	asm volatile("out" #bwl " %" #bw "0, %w1"			\
		     : : "a"(value), "Nd"(port));			\
	if (_tracepoint_enabled(portio_write))				\
		do_trace_portio_write(value, port, #bwl[0]);		\
}									\
									\
static inline type __in##bwl(u16 port)					\
{									\
	type value;							\
	asm volatile("in" #bwl " %w1, %" #bw "0"			\
		     : "=a"(value) : "Nd"(port));			\
	if (_tracepoint_enabled(portio_read))				\
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
