/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_PAGE_SECTION_H
#define _LINUX_MM_PAGE_SECTION_H

#include <linux/mm_types.h> // for struct page
#include <linux/mmzone.h> // for SECTIONS_*

#if defined(CONFIG_SPARSEMEM) && !defined(CONFIG_SPARSEMEM_VMEMMAP)
#define SECTION_IN_PAGE_FLAGS

static inline void set_page_section(struct page *page, unsigned long section)
{
	page->flags &= ~(SECTIONS_MASK << SECTIONS_PGSHIFT);
	page->flags |= (section & SECTIONS_MASK) << SECTIONS_PGSHIFT;
}

static inline unsigned long page_to_section(const struct page *page)
{
	return (page->flags >> SECTIONS_PGSHIFT) & SECTIONS_MASK;
}
#endif

#endif /* _LINUX_MM_PAGE_SECTION_H */
