// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/copypage.c
 *
 * Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitops.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/mte.h>
#include <asm/mte_tag_storage.h>

#ifdef CONFIG_ARM64_MTE_TAG_STORAGE
static inline bool try_transfer_saved_tags(struct page *from, struct page *to)
{
	void *tags;
	bool saved;

	VM_WARN_ON_ONCE(!preemptible());

	if (page_mte_tagged(from)) {
		if (likely(page_tag_storage_reserved(to)))
			return false;

		tags = mte_allocate_tag_buf();
		if (WARN_ON(!tags))
			return true;

		mte_copy_page_tags_to_buf(page_address(from), tags);
		saved = mte_save_tags_for_pfn(tags, page_to_pfn(to));
		if (!saved)
			mte_free_tag_buf(tags);

		return saved;
	}

	if (likely(!page_is_tag_storage(from)))
		return false;

	tags_by_pfn_lock();
	tags = mte_erase_tags_for_pfn(page_to_pfn(from));
	tags_by_pfn_unlock();

	if (likely(!tags))
		return false;

	if (page_tag_storage_reserved(to)) {
		WARN_ON_ONCE(!try_page_mte_tagging(to));
		mte_copy_page_tags_from_buf(page_address(to), tags);
		set_page_mte_tagged(to);
		mte_free_tag_buf(tags);
		return true;
	}

	saved = mte_save_tags_for_pfn(tags, page_to_pfn(to));
	if (!saved)
		mte_free_tag_buf(tags);

	return saved;
}
#else
static inline bool try_transfer_saved_tags(struct page *from, struct page *to)
{
	return false;
}
#endif

void copy_highpage(struct page *to, struct page *from)
{
	void *kto = page_address(to);
	void *kfrom = page_address(from);

	copy_page(kto, kfrom);

	if (kasan_hw_tags_enabled())
		page_kasan_tag_reset(to);

	if (tag_storage_enabled() && try_transfer_saved_tags(from, to))
		return;

	if (system_supports_mte() && page_mte_tagged(from)) {
		/* It's a new page, shouldn't have been tagged yet */
		WARN_ON_ONCE(!try_page_mte_tagging(to));
		mte_copy_page_tags(kto, kfrom);
		set_page_mte_tagged(to);
	}
}
EXPORT_SYMBOL(copy_highpage);

void copy_user_highpage(struct page *to, struct page *from,
			unsigned long vaddr, struct vm_area_struct *vma)
{
	copy_highpage(to, from);
	flush_dcache_page(to);
}
EXPORT_SYMBOL_GPL(copy_user_highpage);
