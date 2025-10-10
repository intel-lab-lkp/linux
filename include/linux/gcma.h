/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GCMA_H__
#define __GCMA_H__

#include <linux/types.h>

#ifdef CONFIG_GCMA

int gcma_register_area(const char *name,
		       unsigned long start_pfn, unsigned long count);

/*
 * NOTE: allocated pages are still marked reserved and when freeing them
 * the caller should ensure they are isolated and not referenced by anyone
 * other than the caller.
 */
int gcma_alloc_range(unsigned long start_pfn, unsigned long count, gfp_t gfp);
int gcma_free_range(unsigned long start_pfn, unsigned long count);

#else /* CONFIG_GCMA */

static inline int gcma_register_area(const char *name,
				     unsigned long start_pfn,
				     unsigned long count)
		{ return -EOPNOTSUPP; }
static inline int gcma_alloc_range(unsigned long start_pfn,
				   unsigned long count, gfp_t gfp)
		{ return -EOPNOTSUPP; }

static inline int gcma_free_range(unsigned long start_pfn,
				   unsigned long count)
		{ return -EOPNOTSUPP; }

#endif /* CONFIG_GCMA */

#endif /* __GCMA_H__ */
