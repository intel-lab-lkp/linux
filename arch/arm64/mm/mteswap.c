// SPDX-License-Identifier: GPL-2.0-only

#include <linux/pagemap.h>
#include <linux/xarray.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <asm/mte.h>

static DEFINE_XARRAY(tags_by_swp_entry);

void *mte_allocate_tag_buf(void)
{
	/* tags granule is 16 bytes, 2 tags stored per byte */
	return kmalloc(MTE_PAGE_TAG_STORAGE_SIZE, GFP_KERNEL);
}

void mte_free_tag_buf(void *buf)
{
	kfree(buf);
}

#ifdef CONFIG_ARM64_MTE_TAG_STORAGE
static DEFINE_XARRAY(tags_by_pfn);

void tags_by_pfn_lock(void)
{
	xa_lock(&tags_by_pfn);
}

void tags_by_pfn_unlock(void)
{
	xa_unlock(&tags_by_pfn);
}

void *mte_erase_tags_for_pfn(unsigned long pfn)
{
	return __xa_erase(&tags_by_pfn, pfn);
}

bool mte_save_tags_for_pfn(void *tags, unsigned long pfn)
{
	void *entry;
	int ret;

	ret = xa_reserve(&tags_by_pfn, pfn, GFP_KERNEL);
	if (ret)
		return true;

	tags_by_pfn_lock();

	if (page_tag_storage_reserved(pfn_to_page(pfn))) {
		tags_by_pfn_unlock();
		return false;
	}

	entry = __xa_store(&tags_by_pfn, pfn, tags, GFP_ATOMIC);
	if (xa_is_err(entry)) {
		xa_release(&tags_by_pfn, pfn);
		goto out_unlock;
	} else if (entry) {
		mte_free_tag_buf(entry);
	}

out_unlock:
	tags_by_pfn_unlock();
	return true;
}

void mte_restore_tags_for_pfn(unsigned long start_pfn, int order)
{
	struct page *page = pfn_to_page(start_pfn);
	unsigned long pfn;
	void *tags;

	tags_by_pfn_lock();

	for (pfn = start_pfn; pfn < start_pfn + (1 << order); pfn++, page++) {
		if (WARN_ON_ONCE(!page_tag_storage_reserved(page)))
			continue;

		tags = mte_erase_tags_for_pfn(pfn);
		if (unlikely(tags)) {
			/*
			 * Mark the page as tagged so mte_sync_tags() doesn't
			 * clear the tags.
			 */
			WARN_ON_ONCE(!try_page_mte_tagging(page));
			mte_copy_page_tags_from_buf(page_address(page), tags);
			set_page_mte_tagged(page);
			mte_free_tag_buf(tags);
		}
	}

	tags_by_pfn_unlock();
}

/*
 * Note on locking: swap in/out is done with the folio locked, which eliminates
 * races with mte_save/restore_page_tags_by_swp_entry.
 */
vm_fault_t mte_try_transfer_swap_tags(swp_entry_t entry, struct page *page)
{
	void *swap_tags, *pfn_tags;
	bool saved;

	/*
	 * mte_restore_page_tags_by_swp_entry() will take care of copying the
	 * tags over.
	 */
	if (likely(page_mte_tagged(page) || page_tag_storage_reserved(page)))
		return 0;

	swap_tags = xa_load(&tags_by_swp_entry, entry.val);
	if (!swap_tags)
		return 0;

	pfn_tags = mte_allocate_tag_buf();
	if (!pfn_tags)
		return VM_FAULT_OOM;

	memcpy(pfn_tags, swap_tags, MTE_PAGE_TAG_STORAGE_SIZE);
	saved = mte_save_tags_for_pfn(pfn_tags, page_to_pfn(page));
	if (!saved)
		mte_free_tag_buf(pfn_tags);

	return 0;
}
#endif

int mte_save_page_tags_by_swp_entry(struct page *page)
{
	void *tags, *ret;

	if (!page_mte_tagged(page))
		return 0;

	tags = mte_allocate_tag_buf();
	if (!tags)
		return -ENOMEM;

	mte_copy_page_tags_to_buf(page_address(page), tags);

	/* lookup the swap entry.val from the page */
	ret = xa_store(&tags_by_swp_entry, page_swap_entry(page).val, tags,
		       GFP_KERNEL);
	if (WARN(xa_is_err(ret), "Failed to store MTE tags")) {
		mte_free_tag_buf(tags);
		return xa_err(ret);
	} else if (ret) {
		/* Entry is being replaced, free the old entry */
		mte_free_tag_buf(ret);
	}

	return 0;
}

void mte_restore_page_tags_by_swp_entry(swp_entry_t entry, struct page *page)
{
	void *tags = xa_load(&tags_by_swp_entry, entry.val);

	if (!tags)
		return;

	/* Tags will be restored when tag storage is reserved. */
	if (tag_storage_enabled() && unlikely(!page_tag_storage_reserved(page)))
		return;

	if (try_page_mte_tagging(page)) {
		mte_copy_page_tags_from_buf(page_address(page), tags);
		set_page_mte_tagged(page);
	}
}

void mte_invalidate_tags_by_swp_entry(int type, pgoff_t offset)
{
	swp_entry_t entry = swp_entry(type, offset);
	void *tags = xa_erase(&tags_by_swp_entry, entry.val);

	mte_free_tag_buf(tags);
}

void mte_invalidate_tags_area_by_swp_entry(int type)
{
	swp_entry_t entry = swp_entry(type, 0);
	swp_entry_t last_entry = swp_entry(type + 1, 0);
	void *tags;

	XA_STATE(xa_state, &tags_by_swp_entry, entry.val);

	xa_lock(&tags_by_swp_entry);
	xas_for_each(&xa_state, tags, last_entry.val - 1) {
		__xa_erase(&tags_by_swp_entry, xa_state.xa_index);
		mte_free_tag_buf(tags);
	}
	xa_unlock(&tags_by_swp_entry);
}
