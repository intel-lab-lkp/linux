/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2017, Michael Ellerman, IBM Corporation.
 */
#ifndef _LINUX_SET_MEMORY_H_
#define _LINUX_SET_MEMORY_H_

#ifdef CONFIG_ARCH_HAS_SET_MEMORY
#include <asm/set_memory.h>
#else
static inline int __must_check set_memory_ro(unsigned long addr, int numpages) { return 0; }
static inline int __must_check set_memory_rw(unsigned long addr, int numpages) { return 0; }
static inline int __must_check set_memory_x(unsigned long addr,  int numpages) { return 0; }
static inline int __must_check set_memory_nx(unsigned long addr, int numpages) { return 0; }
#endif

#ifndef set_memory_rox
static inline int set_memory_rox(unsigned long addr, int numpages)
{
	int ret = set_memory_ro(addr, numpages);
	if (ret)
		return ret;
	return set_memory_x(addr, numpages);
}
#endif

#ifndef CONFIG_ARCH_HAS_SET_DIRECT_MAP
static inline int set_direct_map_invalid_noflush(struct page *page)
{
	return 0;
}
static inline int set_direct_map_default_noflush(struct page *page)
{
	return 0;
}

static inline int set_direct_map_valid_noflush(struct page *page,
					       unsigned nr, bool valid)
{
	return 0;
}

/**
 * set_direct_map_ro_noflush - make a direct-map range read-only
 * @addr: start address in the direct map
 * @nr_pages: number of pages starting at @addr
 *
 * Make the direct-map range starting at @addr read-only without invalidating
 * TLBs. Callers must either ensure that no stale writable translations can
 * be used, or treat the permission change as a best-effort hardening step.
 *
 * Return: 0 on success or when direct-map permission changes are unsupported,
 * or a negative errno on failure.
 */
static inline int set_direct_map_ro_noflush(const void *addr,
					    unsigned long nr_pages)
{
	return 0;
}

static inline bool kernel_page_present(struct page *page)
{
	return true;
}
#else /* CONFIG_ARCH_HAS_SET_DIRECT_MAP */
/*
 * Some architectures, e.g. ARM64 can disable direct map modifications at
 * boot time. Let them overrive this query.
 */
#ifndef can_set_direct_map
static inline bool can_set_direct_map(void)
{
	return true;
}
#define can_set_direct_map can_set_direct_map
#endif

#ifndef set_direct_map_ro_noflush
/* See the comment above the generic fallback for the _noflush contract. */
static inline int set_direct_map_ro_noflush(const void *addr,
					    unsigned long nr_pages)
{
	return 0;
}

#define set_direct_map_ro_noflush set_direct_map_ro_noflush
#endif
#endif /* CONFIG_ARCH_HAS_SET_DIRECT_MAP */

#ifdef CONFIG_X86_64
int set_mce_nospec(unsigned long pfn);
int clear_mce_nospec(unsigned long pfn);
#else
static inline int set_mce_nospec(unsigned long pfn)
{
	return 0;
}
static inline int clear_mce_nospec(unsigned long pfn)
{
	return 0;
}
#endif

#ifndef CONFIG_ARCH_HAS_MEM_ENCRYPT
static inline int set_memory_encrypted(unsigned long addr, int numpages)
{
	return 0;
}

static inline int set_memory_decrypted(unsigned long addr, int numpages)
{
	return 0;
}
#endif /* CONFIG_ARCH_HAS_MEM_ENCRYPT */

#endif /* _LINUX_SET_MEMORY_H_ */
