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
 * @get_pfn: Obtain a PFN for a given index. The provider must ensure backing
 *           memory is present and is responsible for its contents, such as
 *           zeroing if necessary. On success, return 0 with the physical frame
 *           number in @pfn and, if @max_order is non-NULL, the maximum page
 *           order of contiguous backing memory containing @pfn. Returns a
 *           negative error code on failure. The provider must define
 *           @lookup_pfn_range when @get_pfn is defined.
 * @lookup_pfn_range: Look up the PFN for a given index. Returns 0 on success
 *                    with the physical frame number in @pfn and the number of
 *                    contiguous pages the mapping is valid for in @nr_pages.
 *                    The provider does not need to ensure that backing memory
 *                    is present. If backing memory is not available, return
 *                    -ENOENT. Return a negative error code on other failure.
 *                    The provider must define @get_pfn when @lookup_pfn_range
 *                    is defined.
 *
 * Used by filesystems and device drivers that provide memory for guest_memfd.
 */
struct guest_memfd_provider_operations {
	void *(*attach)(struct inode *gmem_inode, struct file *resource_file);
	void (*release)(void *provider);
	struct folio *(*alloc_folio)(void *provider, pgoff_t index,
				     struct mempolicy *mpol);
	void (*invalidate_folio)(void *provider, struct folio *folio);
	int (*get_pfn)(void *provider, pgoff_t index, unsigned long *pfn,
		       int *max_order);
	int (*lookup_pfn_range)(void *provider, pgoff_t index,
				unsigned long *pfn, size_t *nr_pages);
};

#endif /* _LINUX_GUEST_MEMFD_H */
