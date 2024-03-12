/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_VMALLOC_ADDR_H
#define _LINUX_MM_VMALLOC_ADDR_H

#include <linux/types.h> // for bool

struct page;

/* Support for virtually mapped pages */
struct page *vmalloc_to_page(const void *addr);
unsigned long vmalloc_to_pfn(const void *addr);

/*
 * Determine if an address is within the vmalloc range
 *
 * On nommu, vmalloc/vfree wrap through kmalloc/kfree directly, so there
 * is no special casing required.
 */
#ifdef CONFIG_MMU
extern bool is_vmalloc_addr(const void *x);
extern int is_vmalloc_or_module_addr(const void *x);
#else
static inline bool is_vmalloc_addr(const void *x)
{
	return false;
}
static inline int is_vmalloc_or_module_addr(const void *x)
{
	return 0;
}
#endif

#endif /* _LINUX_MM_VMALLOC_ADDR_H */
