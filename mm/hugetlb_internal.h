/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Pratyush Yadav <pratyush@kernel.org>
 */
#ifndef __HUGETLB_INTERNAL_H
#define __HUGETLB_INTERNAL_H

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/list.h>
#include <linux/liveupdate.h>

void init_new_hugetlb_folio(struct folio *folio);
void account_new_hugetlb_folio(struct hstate *h, struct folio *folio);

long region_chg(struct resv_map *resv, long f, long t, long *out_regions_needed);
long region_add(struct resv_map *resv, long f, long t, long in_regions_needed,
		struct hstate *h, struct hugetlb_cgroup *h_cg);
void region_abort(struct resv_map *resv, long f, long t, long regions_needed);
void prep_and_add_allocated_folios(struct hstate *h, struct list_head *folio_list);

static inline struct resv_map *inode_resv_map(struct inode *inode)
{
	/*
	 * At inode evict time, i_mapping may not point to the original
	 * address space within the inode.  This original address space
	 * contains the pointer to the resv_map.  So, always use the
	 * address space embedded within the inode.
	 * The VERY common case is inode->mapping == &inode->i_data but,
	 * this may not be true for device special inodes.
	 */
	return (struct resv_map *)(&inode->i_data)->i_private_data;
}

#ifdef CONFIG_LIVEUPDATE_HUGETLB
void hugetlb_luo_init(void);
unsigned long hstate_liveupdate_pages(struct hstate *h);
#else
static inline void hugetlb_luo_init(void)
{
}

static inline unsigned long hstate_liveupdate_pages(struct hstate *h)
{
	return 0;
}
#endif /* CONFIG_LIVEUPDATE_HUGETLB */

#endif /* __HUGETLB_INTERNAL_H */
