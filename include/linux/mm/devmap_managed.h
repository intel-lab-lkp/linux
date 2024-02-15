/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_DEVMAP_MANAGED_H
#define _LINUX_MM_DEVMAP_MANAGED_H

#include <linux/types.h> // for bool

struct page;

#if defined(CONFIG_ZONE_DEVICE) && defined(CONFIG_FS_DAX)

#include <linux/jump_label.h> // for DECLARE_STATIC_KEY_FALSE(), static_branch_unlikely()
#include <linux/mmzone.h> // is_zone_device_page()

DECLARE_STATIC_KEY_FALSE(devmap_managed_key);

bool __put_devmap_managed_page_refs(struct page *page, int refs);
static inline bool put_devmap_managed_page_refs(struct page *page, int refs)
{
	if (!static_branch_unlikely(&devmap_managed_key))
		return false;
	if (!is_zone_device_page(page))
		return false;
	return __put_devmap_managed_page_refs(page, refs);
}
#else /* CONFIG_ZONE_DEVICE && CONFIG_FS_DAX */
static inline bool put_devmap_managed_page_refs(struct page *page, int refs)
{
	return false;
}
#endif /* CONFIG_ZONE_DEVICE && CONFIG_FS_DAX */

static inline bool put_devmap_managed_page(struct page *page)
{
	return put_devmap_managed_page_refs(page, 1);
}

#endif /* _LINUX_MM_DEVMAP_MANAGED_H */
