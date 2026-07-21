/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#ifndef _ASM_LOONGARCH_SET_MEMORY_H
#define _ASM_LOONGARCH_SET_MEMORY_H

/*
 * Functions to change memory attributes.
 */
int set_memory_x(unsigned long addr, int numpages);
int set_memory_nx(unsigned long addr, int numpages);
int set_memory_ro(unsigned long addr, int numpages);
int set_memory_rw(unsigned long addr, int numpages);

bool kernel_page_present(struct page *page);
int set_direct_map_default_noflush(struct page *page);
int set_direct_map_invalid_noflush(struct page *page);
int set_direct_map_valid_noflush(struct page *page, unsigned nr, bool valid);

/*
 * The direct map is a fixed address window that is not backed by page tables,
 * so a page never has a second mapping whose protection must be synchronised.
 */
static inline bool cpa_should_update_alias(unsigned long vaddr, unsigned long pfn)
{
	return false;
}

#endif /* _ASM_LOONGARCH_SET_MEMORY_H */
