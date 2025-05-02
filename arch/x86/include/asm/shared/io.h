/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_IO_H
#define _ASM_X86_SHARED_IO_H

#include <linux/types.h>

#define BUILDIO(bwl, bw, type)						\
static __always_inline void __out##bwl(type value, u16 port)		\
{									\
	asm volatile("out" #bwl " %" #bw "0, %w1"			\
		     : : "a"(value), "Nd"(port));			\
}									\
									\
static __always_inline type __in##bwl(u16 port)				\
{									\
	type value;							\
	asm volatile("in" #bwl " %w1, %" #bw "0"			\
		     : "=a"(value) : "Nd"(port));			\
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

#define build_mmio_read(name, size, type, reg, barrier) \
static inline type name(const volatile void __iomem *addr) \
{ type ret; asm volatile("mov" size " %1,%0":reg (ret) \
:"m" (*(volatile type __force *)addr) barrier); return ret; }

#define build_mmio_write(name, size, type, reg, barrier) \
static inline void name(type val, volatile void __iomem *addr) \
{ asm volatile("mov" size " %0,%1": :reg (val), \
"m" (*(volatile type __force *)addr) barrier); }

build_mmio_read(readb, "b", unsigned char, "=q", :"memory")
build_mmio_read(readw, "w", unsigned short, "=r", :"memory")
build_mmio_read(readl, "l", unsigned int, "=r", :"memory")

build_mmio_read(__readb, "b", unsigned char, "=q", )
build_mmio_read(__readw, "w", unsigned short, "=r", )
build_mmio_read(__readl, "l", unsigned int, "=r", )

build_mmio_write(writeb, "b", unsigned char, "q", :"memory")
build_mmio_write(writew, "w", unsigned short, "r", :"memory")
build_mmio_write(writel, "l", unsigned int, "r", :"memory")

build_mmio_write(__writeb, "b", unsigned char, "q", )
build_mmio_write(__writew, "w", unsigned short, "r", )
build_mmio_write(__writel, "l", unsigned int, "r", )

#define readb readb
#define readw readw
#define readl readl
#define readb_relaxed(a) __readb(a)
#define readw_relaxed(a) __readw(a)
#define readl_relaxed(a) __readl(a)
#define __raw_readb __readb
#define __raw_readw __readw
#define __raw_readl __readl

#define writeb writeb
#define writew writew
#define writel writel
#define writeb_relaxed(v, a) __writeb(v, a)
#define writew_relaxed(v, a) __writew(v, a)
#define writel_relaxed(v, a) __writel(v, a)
#define __raw_writeb __writeb
#define __raw_writew __writew
#define __raw_writel __writel

#ifdef CONFIG_X86_64

build_mmio_read(readq, "q", u64, "=r", :"memory")
build_mmio_read(__readq, "q", u64, "=r", )
build_mmio_write(writeq, "q", u64, "r", :"memory")
build_mmio_write(__writeq, "q", u64, "r", )

#define readq_relaxed(a)	__readq(a)
#define writeq_relaxed(v, a)	__writeq(v, a)

#define __raw_readq		__readq
#define __raw_writeq		__writeq

/* Let people know that we have them */
#define readq			readq
#define writeq			writeq

#endif

#undef build_mmio_write
#undef build_mmio_read

#endif
