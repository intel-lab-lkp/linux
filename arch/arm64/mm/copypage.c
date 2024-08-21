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

void copy_highpage(struct page *to, struct page *from)
{
	void *kto = page_address(to);
	void *kfrom = page_address(from);
	struct folio *src = page_folio(from);
	struct folio *dst = page_folio(to);
	unsigned int i, nr_pages;

	copy_page(kto, kfrom);

	if (kasan_hw_tags_enabled())
		page_kasan_tag_reset(to);

	if (system_supports_mte() && page_mte_tagged(from)) {
		/* It's a new page, shouldn't have been tagged yet */
		WARN_ON_ONCE(!try_page_mte_tagging(to));

		/* Populate tags for all subpages if hugetlb */
		if (folio_test_hugetlb(src)) {
			/*
			 * MTE page flag is just set on the head page of
			 * hugetlb. If from has MTE flag set, it must be the
			 * head page.
			 */
			VM_BUG_ON(!PageHead(from));
			nr_pages = folio_nr_pages(src);
			for (i = 0; i < nr_pages; i++, to++, from++) {
				kto = page_address(to);
				kfrom = page_address(from);
				mte_copy_page_tags(kto, kfrom);
			}
			set_page_mte_tagged(&dst->page);
		} else {
			mte_copy_page_tags(kto, kfrom);
			set_page_mte_tagged(to);
		}
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
