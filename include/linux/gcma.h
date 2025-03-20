/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GCMA_H__
#define __GCMA_H__

#include <linux/types.h>

int gcma_register_area(const char *name,
		       unsigned long start_pfn, unsigned long count);
void gcma_alloc_range(unsigned long start_pfn, unsigned long count);
void gcma_free_range(unsigned long start_pfn, unsigned long count);

#endif /* __GCMA_H__ */
