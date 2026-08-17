/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_STACKDEPOT_H
#define _ASM_X86_STACKDEPOT_H

#include <linux/types.h>

#ifdef CONFIG_X86_64
/*
 * Compress canonical kernel text/module addresses whose upper 32 bits are all
 * ones. Other kernel virtual addresses stay raw, so decompression reconstructs
 * the original frame by restoring this prefix.
 */
#define STACK_DEPOT_X86_64_FRAME_PREFIX	0xffffffff00000000UL
#define STACK_DEPOT_X86_64_FRAME_LOW_MASK	0x00000000ffffffffUL

static inline bool
arch_stack_depot_frame_try_compress(unsigned long frame, u32 *low)
{
	if ((frame & ~STACK_DEPOT_X86_64_FRAME_LOW_MASK) !=
	    STACK_DEPOT_X86_64_FRAME_PREFIX)
		return false;

	*low = (u32)frame;
	return true;
}

static inline void
arch_stack_depot_frame_decompress(u32 low, unsigned long *frame)
{
	*frame = STACK_DEPOT_X86_64_FRAME_PREFIX | low;
}

#else
#include <asm-generic/stackdepot.h>
#endif /* CONFIG_X86_64 */

#endif /* _ASM_X86_STACKDEPOT_H */
