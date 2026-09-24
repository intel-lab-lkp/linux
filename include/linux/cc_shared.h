/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CC_SHARED_H
#define _LINUX_CC_SHARED_H

#include <linux/gfp_types.h>
#include <linux/types.h>

struct page;

struct cc_shared_pages {
	struct page *page;
	size_t shared_size;
};

struct cc_shared_layout {
	size_t requested_size;
	size_t shared_size;
	size_t alignment;
};

/*
 * Architectures may override this to return the granule used for transitions
 * between private and shared memory. The value must be a power of two and no
 * smaller than PAGE_SIZE.
 */
size_t arch_cc_shared_granule_size(void);

size_t cc_shared_granule_size(void);
int cc_shared_calc_layout(size_t requested, struct cc_shared_layout *layout);
bool cc_shared_range_valid(phys_addr_t base, size_t size);
int cc_make_shared(void *addr, size_t size);
int cc_make_private(void *addr, size_t size);
int alloc_cc_shared_pages_node(int nid, gfp_t gfp,
		size_t requested, struct cc_shared_pages *mem);
int alloc_cc_shared_pages(gfp_t gfp,
		size_t requested, struct cc_shared_pages *mem);
void free_cc_shared_pages(struct cc_shared_pages *mem);

#endif /* _LINUX_CC_SHARED_H */
