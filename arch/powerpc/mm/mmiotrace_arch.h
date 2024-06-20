/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from arch/powerpc/mm/pgtable.c:
 *   Copyright (C) 2024 Jialong Yang (jialong.yang@shingroup.cn)
 */

#ifndef __MMIOTRACE_ARCH_
#define __MMIOTRACE_ARCH_
#include <asm/pgtable.h>

static inline int page_level_shift(unsigned int level)
{
	return level;
}
static inline unsigned long page_level_size(unsigned int level)
{
	return 1UL << page_level_shift(level);
}
static inline unsigned long page_level_mask(unsigned int level)
{
	return ~(page_level_size(level) - 1);
}

pte_t *lookup_address(unsigned long address, unsigned int *level);
#endif // __MMIOTRACE_ARCH_
