/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUEST_MEMFD_H
#define _LINUX_GUEST_MEMFD_H

#include <linux/fs.h>

/**
 * struct guest_memfd_operations - ops provided by owner to manage folios
 * @invalidate_begin: called when folios should be unmapped from guest.
 *                    May fail if folios couldn't be unmapped from guest.
 *                    Required.
 * @invalidate_end: called after invalidate_begin returns success. Optional.
 * @prepare: called before a folio is mapped into the guest address space.
 *           Optional.
 * @accessible: called after prepare returns success and before it's mapped
 *              into the guest address space. Returns 0 if the folio can be
 *              accessed.
 *              Optional. If not present, assumes folios are never accessible.
 * @release: Called when releasing the guest_memfd file. Required.
 */
struct guest_memfd_operations {
	int (*invalidate_begin)(struct inode *inode, pgoff_t offset, unsigned long nr);
	void (*invalidate_end)(struct inode *inode, pgoff_t offset, unsigned long nr);
	int (*prepare)(struct inode *inode, pgoff_t offset, struct folio *folio);
	int (*accessible)(struct inode *inode, struct folio *folio,
			  pgoff_t offset, unsigned long nr);
	int (*release)(struct inode *inode);
};

/**
 * @GUEST_MEMFD_FLAG_NO_DIRECT_MAP: When making folios inaccessible by host, also
 *                                  remove them from the kernel's direct map.
 */
enum {
	GUEST_MEMFD_FLAG_NO_DIRECT_MAP		= BIT(0),
};

/**
 * @GUEST_MEMFD_GRAB_UPTODATE: Ensure pages are zeroed/up to date.
 *                             If trusted hyp will do it, can ommit this flag
 * @GUEST_MEMFD_PREPARE: Call the ->prepare() op, if present.
 */
enum {
	GUEST_MEMFD_GRAB_UPTODATE	= BIT(0),
	GUEST_MEMFD_PREPARE		= BIT(1),
};

struct folio *guest_memfd_grab_folio(struct file *file, pgoff_t index, u32 flags);
struct file *guest_memfd_alloc(const char *name,
			       const struct guest_memfd_operations *ops,
			       loff_t size, unsigned long flags);
bool is_guest_memfd(struct file *file, const struct guest_memfd_operations *ops);
int guest_memfd_make_inaccessible(struct file *file, struct folio *folio);

#endif
