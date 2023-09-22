// SPDX-License-Identifier: GPL-2.0

/* MTE tag storage management without compression support. */

#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/xarray.h>

#include <asm/mte.h>

#include "mteswap.h"

void *_mte_alloc_and_save_tags(struct page *page)
{
	void *storage;

	storage = mte_allocate_tag_storage();
	if (!storage)
		return NULL;

	mte_save_page_tags(page_address(page), storage);
	return storage;
}

void _mte_free_saved_tags(void *storage)
{
	mte_free_tag_storage(storage);
}

void _mte_restore_tags(void *tags, struct page *page)
{
	if (!try_page_mte_tagging(page))
		return;
	mte_restore_page_tags(page_address(page), tags);
	set_page_mte_tagged(page);
}
