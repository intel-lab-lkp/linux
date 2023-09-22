// SPDX-License-Identifier: GPL-2.0

/* MTE tag storage management with compression. */

#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/xarray.h>

#include <asm/mte.h>
#include <asm/mtecomp.h>

#include "mteswap.h"

void *_mte_alloc_and_save_tags(struct page *page)
{
	unsigned long handle;
	u8 *tags;

	tags = mte_allocate_tag_storage();
	if (!tags)
		return xa_mk_value(0);
	mte_save_page_tags(page_address(page), tags);
	handle = mte_compress(tags);
	mte_free_tag_storage(tags);
	return xa_mk_value(handle);
}

void _mte_free_saved_tags(void *storage)
{
	unsigned long handle;

	handle = xa_to_value(storage);
	if (!handle)
		return;
	mte_release_handle(handle);
}

void _mte_restore_tags(void *tags, struct page *page)
{
	unsigned long handle;
	u8 *tags_decomp;

	handle = xa_to_value(tags);
	if (!handle)
		return;
	if (!try_page_mte_tagging(page))
		return;
	tags_decomp = mte_allocate_tag_storage();
	if (!tags_decomp)
		return;
	if (!mte_decompress(handle, tags_decomp))
		return;
	mte_restore_page_tags(page_address(page), tags_decomp);
	set_page_mte_tagged(page);
	mte_free_tag_storage(tags_decomp);
}
MODULE_IMPORT_NS(MTECOMP);
