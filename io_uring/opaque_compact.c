// SPDX-License-Identifier: GPL-2.0
/* Workers copy immutable backing without holding object table locks. */
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "io_uring.h"
#include "opaque_internal.h"

struct page *io_opaque_owned_page(unsigned int *order)
{
	struct page *page;

	for (;;) {
		page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_COMP | __GFP_NORETRY |
				   __GFP_NOWARN, *order);
		if (page || !*order)
			return page;
		(*order)--;
	}
}

/* Destination allocation for dense copies and CREATE. */
struct page *io_opaque_dest_page(struct io_opaque_store *store, u64 pages_left,
				 unsigned int *order)
{
	*order = min_t(unsigned int, IO_OPAQUE_MAX_ORDER, ilog2(pages_left));
	return io_opaque_owned_page(order);
}
