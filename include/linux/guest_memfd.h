/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_GUEST_MEMFD_H
#define _LINUX_GUEST_MEMFD_H

#include <linux/types.h>

struct file;
struct folio;
struct inode;
struct mempolicy;

/**
 * struct guest_memfd_provider_operations - Operations for external memory providers
 * @attach: Attach to a resource file, perform filesystem-specific validation,
 *          and return an opaque provider context.
 * @release: Release provider context and unpin any resources.
 * @alloc_folio: Allocate an uninserted folio for a given index and NUMA policy.
 *               Must return a pointer to a struct folio on success, or an
 *               ERR_PTR() error code on failure. Must never return NULL.
 * @invalidate_folio: Notify provider that a folio has been invalidated.
 *
 * Used by filesystems and device drivers that provide memory for guest_memfd.
 */
struct guest_memfd_provider_operations {
	void *(*attach)(struct inode *gmem_inode, struct file *resource_file);
	void (*release)(void *provider);
	struct folio *(*alloc_folio)(void *provider, pgoff_t index,
				     struct mempolicy *mpol);
	void (*invalidate_folio)(void *provider, struct folio *folio);
};

#endif /* _LINUX_GUEST_MEMFD_H */
