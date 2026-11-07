/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_GUEST_MEMFD_H
#define _LINUX_GUEST_MEMFD_H

#include <linux/bits.h>
#include <linux/types.h>

struct file;
struct folio;
struct inode;
struct mempolicy;

#define GUEST_MEMFD_PFN_ATTRS_READONLY		(1ULL << 0)

/*
 * Cacheability attributes for physical frames returned by providers
 * (columns show x86 / arm64 memory types):
 *
 * Attribute                               | Host PTE       | Stage-2 PTE
 * ----------------------------------------+----------------+---------------
 * GUEST_MEMFD_PFN_ATTRS_CACHE_WB          | WB / Normal    | WB / Normal
 * GUEST_MEMFD_PFN_ATTRS_CACHE_UC          | UC / Device    | UC / Device
 * GUEST_MEMFD_PFN_ATTRS_CACHE_UC_ALLOW_WC | UC / Device    | UC / Normal-NC
 * GUEST_MEMFD_PFN_ATTRS_CACHE_WC          | WC / Normal-NC | UC / Normal-NC
 *
 * GUEST_MEMFD_PFN_ATTRS_CACHE_WB is 0 so normal cacheable memory is the
 * default when no cacheability bits are set.
 */
#define GUEST_MEMFD_PFN_ATTRS_CACHE_MASK	GENMASK_ULL(2, 1)
#define GUEST_MEMFD_PFN_ATTRS_CACHE_WB		(0ULL << 1)
#define GUEST_MEMFD_PFN_ATTRS_CACHE_UC		(1ULL << 1)
#define GUEST_MEMFD_PFN_ATTRS_CACHE_UC_ALLOW_WC	(2ULL << 1)
#define GUEST_MEMFD_PFN_ATTRS_CACHE_WC		(3ULL << 1)

static inline bool kvm_gmem_pfn_is_cacheable(u64 pfn_attrs)
{
	return (pfn_attrs & GUEST_MEMFD_PFN_ATTRS_CACHE_MASK) ==
	       GUEST_MEMFD_PFN_ATTRS_CACHE_WB;
}

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
 *           number in @pfn and, if non-NULL, the PFN attributes in @pfn_attrs
 *           and the maximum page order of contiguous backing memory containing
 *           @pfn with uniform @pfn_attrs in @max_order. Once @get_pfn succeeds,
 *           the provider must keep the @index-to-@pfn mapping and @pfn_attrs
 *           constant, and the memory accessible, until guest_memfd stops using
 *           this page. Do not take locks held across kvm_gmem_revoke().
 *           Returns a negative error code on failure. The provider must define
 *           @lookup_pfn_range when @get_pfn is defined.
 * @mmap: Optional callback to set up user mappings. Called when userspace calls
 *        mmap() on guest_memfd. The provider can customize VMA flags,
 *        cacheability/protections, or reject the mapping. If not defined,
 *        guest_memfd falls back to default mapping behavior.
 * @lookup_pfn_range: Look up the PFN for a given index. This only checks for
 *                    the existence of backing pages and should not allocate
 *                    memory. Returns 0 on success with the physical frame
 *                    number in @pfn and the number of physically contiguous
 *                    pages in @nr_pages (@nr_pages need not be reduced for
 *                    pages with different PFN attributes). Once
 *                    @lookup_pfn_range succeeds, the provider must keep the
 *                    existence of the pages and the @index-to-@pfn mapping
 *                    constant until guest_memfd stops using the range. If
 *                    backing memory is not available at @index, return -ENOENT
 *                    with @nr_pages set to the number of consecutive indices
 *                    starting from @index that have no entry. Do not take locks
 *                    held across kvm_gmem_revoke(). Return a negative error
 *                    code on other failure. The provider must define @get_pfn
 *                    when @lookup_pfn_range is defined.
 * @put_pfn_range: Release a range of PFNs previously obtained for a given
 *                 index. Called with the exclusive filemap invalidate lock held.
 *                 This indicates to the provider that guest_memfd is no longer
 *                 using the memory range. The provider does not need to ensure
 *                 that backing memory is present; if backing memory is not
 *                 available, the provider should skip the unbacked range. Not
 *                 called on kvm_gmem_revoke(). Do not take locks held across
 *                 kvm_gmem_revoke().
 * @mmap: Optional callback to set up user mappings. Called when userspace calls
 *        mmap() on guest_memfd. The provider can customize VMA flags, or reject
 *        the mapping.
 * @check_set_attributes: Optional callback to validate attribute changes on a
 *                        range of indices before they are committed. Called
 *                        with the exclusive filemap invalidate lock held. The
 *                        provider can inspect the requested attributes and
 *                        offset range and return 0 to allow the transition or
 *                        a negative error code (e.g. -EINVAL or -EPERM) to
 *                        reject it. Once @check_set_attributes succeeds, the
 *                        provider must ensure this result stays valid and
 *                        constant until guest_memfd stops using the range. Do
 *                        not take locks held across kvm_gmem_revoke().
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
		       int *max_order, u64 *pfn_attrs);
	int (*lookup_pfn_range)(void *provider, pgoff_t index,
				unsigned long *pfn, size_t *nr_pages);
	void (*put_pfn_range)(void *provider, pgoff_t index, size_t nr_pages);
	int (*mmap)(void *provider, struct vm_area_struct *vma);
	int (*check_set_attributes)(void *provider, pgoff_t index,
				    size_t nr_pages, u64 attributes);
};

#ifdef CONFIG_KVM_GUEST_MEMFD
int kvm_gmem_revoke(struct inode *inode, pgoff_t start, size_t nr_pages,
		    const unsigned long *pfn);
#else
static inline int kvm_gmem_revoke(struct inode *inode, pgoff_t start, size_t nr_pages,
				  const unsigned long *pfn)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_GUEST_MEMFD_H */
