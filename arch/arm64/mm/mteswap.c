// SPDX-License-Identifier: GPL-2.0-only

#include <linux/debugfs.h>
#include <linux/pagemap.h>
#include <linux/xarray.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <asm/mte.h>
#include <asm/mtecomp.h>
#include "mtecomp.h"

enum mteswap_counters {
	MTESWAP_CTR_INLINE = 0,
	MTESWAP_CTR_NOINLINE,
	MTESWAP_CTR_SIZE
};
static atomic_long_t alloc_counters[MTESWAP_CTR_SIZE];
static atomic_long_t dealloc_counters[MTESWAP_CTR_SIZE];

static DEFINE_XARRAY(mte_pages);

void *mte_allocate_tag_storage(void)
{
	void *ret;

	ret = kmalloc(MTE_PAGE_TAG_STORAGE, GFP_KERNEL);
	if (ret)
		atomic_long_inc(&alloc_counters[MTESWAP_CTR_NOINLINE]);
	return ret;
}

void mte_free_tag_storage(char *storage)
{
	if (!mte_is_compressed(storage)) {
		kfree(storage);
		atomic_long_dec(&alloc_counters[MTESWAP_CTR_NOINLINE]);
	} else {
		atomic_long_dec(&alloc_counters[MTESWAP_CTR_INLINE]);
	}
}

int mte_save_tags(struct page *page)
{
	void *tag_storage, *ret, *compressed;

	if (!page_mte_tagged(page))
		return 0;

	tag_storage = mte_allocate_tag_storage();
	if (!tag_storage)
		return -ENOMEM;

	mte_save_page_tags(page_address(page), tag_storage);
	compressed = mte_compress(tag_storage);
	if (compressed) {
		mte_free_tag_storage(tag_storage);
		tag_storage = (void *)compressed;
		atomic_long_inc(&alloc_counters[MTESWAP_CTR_INLINE]);
	}

	/* lookup the swap entry.val from the page */
	ret = xa_store(&mte_pages, page_swap_entry(page).val, tag_storage,
		       GFP_KERNEL);
	if (WARN(xa_is_err(ret), "Failed to store MTE tags")) {
		mte_free_tag_storage(tag_storage);
		return xa_err(ret);
	} else if (ret) {
		/* Entry is being replaced, free the old entry */
		mte_free_tag_storage(ret);
	}

	return 0;
}

void mte_restore_tags(swp_entry_t entry, struct page *page)
{
	void *tags = xa_load(&mte_pages, entry.val);
	void *tag_storage = NULL;

	if (!tags)
		return;

	if (try_page_mte_tagging(page)) {
		if (mte_is_compressed(tags)) {
			tag_storage = mte_allocate_tag_storage();
			mte_decompress(tags, tag_storage);
			tags = tag_storage;
		}
		mte_restore_page_tags(page_address(page), tags);
		set_page_mte_tagged(page);
		mte_free_tag_storage(tag_storage);
	}
}

void mte_invalidate_tags(int type, pgoff_t offset)
{
	swp_entry_t entry = swp_entry(type, offset);
	void *tags = xa_erase(&mte_pages, entry.val);

	mte_free_tag_storage(tags);
}

void mte_invalidate_tags_area(int type)
{
	swp_entry_t entry = swp_entry(type, 0);
	swp_entry_t last_entry = swp_entry(type + 1, 0);
	void *tags;

	XA_STATE(xa_state, &mte_pages, entry.val);

	xa_lock(&mte_pages);
	xas_for_each(&xa_state, tags, last_entry.val - 1) {
		__xa_erase(&mte_pages, xa_state.xa_index);
		mte_free_tag_storage(tags);
	}
	xa_unlock(&mte_pages);
}

/* DebugFS interface. */
static int stats_show(struct seq_file *seq, void *v)
{
	unsigned long total_mem_alloc = 0, total_mem_dealloc = 0;
	unsigned long total_num_alloc = 0, total_num_dealloc = 0;
	unsigned long sizes[2] = { 8, MTE_PAGE_TAG_STORAGE };
	long alloc, dealloc;
	unsigned long size;
	int i;

	for (i = 0; i < MTESWAP_CTR_SIZE; i++) {
		alloc = atomic_long_read(&alloc_counters[i]);
		dealloc = atomic_long_read(&dealloc_counters[i]);
		total_num_alloc += alloc;
		total_num_dealloc += dealloc;
		size = sizes[i];
		/*
		 * Do not count 8-byte buffers towards compressed tag storage
		 * size.
		 */
		if (i) {
			total_mem_alloc += (size * alloc);
			total_mem_dealloc += (size * dealloc);
		}
		seq_printf(seq,
			   "%lu bytes: %lu allocations, %lu deallocations\n",
			   size, alloc, dealloc);
	}
	seq_printf(seq, "uncompressed tag storage size: %lu\n",
		   (total_num_alloc - total_num_dealloc) *
			   MTE_PAGE_TAG_STORAGE);
	seq_printf(seq, "compressed tag storage size: %lu\n",
		   total_mem_alloc - total_mem_dealloc);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int mteswap_init(void)
{
	struct dentry *mteswap_dir;

	mteswap_dir = debugfs_create_dir("mteswap", NULL);
	debugfs_create_file("stats", 0444, mteswap_dir, NULL, &stats_fops);
	return 0;
}
module_init(mteswap_init);
