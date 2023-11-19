/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */
#ifndef __ASM_MTE_TAG_STORAGE_H
#define __ASM_MTE_TAG_STORAGE_H

#ifndef __ASSEMBLY__

#include <linux/mm_types.h>

#include <asm/mte.h>

extern void dcache_inval_tags_poc(unsigned long start, unsigned long end);

#ifdef CONFIG_ARM64_MTE_TAG_STORAGE

DECLARE_STATIC_KEY_FALSE(tag_storage_enabled_key);

static inline bool tag_storage_enabled(void)
{
	return static_branch_likely(&tag_storage_enabled_key);
}

static inline bool alloc_requires_tag_storage(gfp_t gfp)
{
	return gfp & __GFP_TAGGED;
}

void mte_tag_storage_init(void);

int reserve_tag_storage(struct page *page, int order, gfp_t gfp);
void free_tag_storage(struct page *page, int order);

bool page_tag_storage_reserved(struct page *page);

vm_fault_t handle_page_missing_tag_storage(struct vm_fault *vmf);
#else
static inline bool tag_storage_enabled(void)
{
	return false;
}
static inline bool alloc_requires_tag_storage(struct page *page)
{
	return false;
}
static inline void mte_tag_storage_init(void)
{
}
static inline int reserve_tag_storage(struct page *page, int order, gfp_t gfp)
{
	return 0;
}
static inline void free_tag_storage(struct page *page, int order)
{
}
static inline bool page_tag_storage_reserved(struct page *page)
{
	return true;
}
#endif /* CONFIG_ARM64_MTE_TAG_STORAGE */

#endif /* !__ASSEMBLY__ */
#endif /* __ASM_MTE_TAG_STORAGE_H  */
