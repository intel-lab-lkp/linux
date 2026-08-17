/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_STACKDEPOT_H
#define __ASM_GENERIC_STACKDEPOT_H

#include <linux/types.h>

static inline bool
arch_stack_depot_frame_try_compress(unsigned long frame, u32 *low)
{
	return false;
}

static inline void
arch_stack_depot_frame_decompress(u32 low, unsigned long *frame)
{
	/* Generic code never compresses frames, so this hook is unreachable. */
}

#endif /* __ASM_GENERIC_STACKDEPOT_H */
