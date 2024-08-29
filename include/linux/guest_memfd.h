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
 * @prepare_inaccessible: called when a folio transitions to inaccessible state
 *                        Optional.
 * @release: Called when releasing the guest_memfd file. Required.
 */
struct guest_memfd_operations {
	int (*invalidate_begin)(struct inode *inode, pgoff_t offset, unsigned long nr);
	void (*invalidate_end)(struct inode *inode, pgoff_t offset, unsigned long nr);
	int (*prepare_inaccessible)(struct inode *inode, struct folio *folio);
	int (*release)(struct inode *inode);
};

enum guest_memfd_create_flags {
	GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE = (1UL << 0),
};

struct folio *guest_memfd_grab_folio(struct file *file, pgoff_t index, u32 flags);
struct file *guest_memfd_alloc(const char *name,
			       const struct guest_memfd_operations *ops,
			       loff_t size, unsigned long flags);
bool is_guest_memfd(struct file *file, const struct guest_memfd_operations *ops);

#endif
