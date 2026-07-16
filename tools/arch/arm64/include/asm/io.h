/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TOOLS_ASM_ARM64_IO_H
#define _TOOLS_ASM_ARM64_IO_H

#define __io_bw()	dma_wmb()
#define __io_ar(v)							\
({									\
	unsigned long tmp;						\
									\
	dma_rmb();							\
									\
	asm volatile("eor	%0, %1, %1\n"				\
		     "cbnz	%0, ."					\
		     : "=r" (tmp) : "r" ((unsigned long)(v))		\
		     : "memory");					\
})

#include <asm-generic/io.h>

#endif /* _TOOLS_ASM_ARM64_IO_H */
