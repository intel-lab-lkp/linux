// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Memory region management for mshv_root module.
 *
 * Authors: Microsoft Linux virtualization team
 */

#include <linux/hmm.h>
#include <linux/hyperv.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include <asm/mshyperv.h>

#include "mshv_root.h"

#define MSHV_MAP_FAULT_IN_PAGES				PTRS_PER_PMD
#define MSHV_INVALID_PFN				ULONG_MAX

/**
 * mshv_chunk_stride - Compute stride for mapping guest memory
 * @page     : The page to check for huge page backing
 * @gfn      : Guest frame number for the mapping
 * @pfn_count: Total number of pages in the mapping
 *
 * Determines the appropriate stride (in pages) for mapping guest memory.
 * Uses huge page stride if the backing page is huge and the guest mapping
 * is properly aligned; otherwise falls back to single page stride.
 *
 * Return: Stride in pages, or -EINVAL if page order is unsupported.
 */
static int mshv_chunk_stride(struct page *page,
			     u64 gfn, u64 pfn_count)
{
	unsigned int page_order;

	/*
	 * Use single page stride by default. For huge page stride, the
	 * page must be compound and point to the head of the compound
	 * page, and both gfn and pfn_count must be huge-page aligned.
	 */
	if (!PageCompound(page) || !PageHead(page) ||
	    !IS_ALIGNED(gfn, PTRS_PER_PMD) ||
	    !IS_ALIGNED(pfn_count, PTRS_PER_PMD))
		return 1;

	page_order = folio_order(page_folio(page));
	/* The hypervisor only supports 2M huge page */
	if (page_order != PMD_ORDER)
		return -EINVAL;

	return 1 << page_order;
}

/**
 * mshv_region_process_chunk - Processes a contiguous chunk of memory pages
 *                             in a region.
 * @region    : Pointer to the memory region structure.
 * @flags     : Flags to pass to the handler.
 * @pfn_offset: Offset into the region's PFNs array to start processing.
 * @pfn_count : Number of PFNs to process.
 * @handler   : Callback function to handle the chunk.
 *
 * This function scans the region's PFNs starting from @pfn_offset,
 * checking for contiguous valid PFNs backed by pages of the same size
 * (normal or huge). It invokes @handler for the chunk of contiguous valid
 * PFNs found. Returns the number of PFNs handled, or a negative error code
 * if the first PFN is invalid or the handler fails.
 *
 * Note: The @handler callback must be able to handle valid PFNs backed by
 * both normal and huge pages.
 *
 * Return: Number of pages handled, or negative error code.
 */
static long mshv_region_process_pfns(struct mshv_mem_region *region,
				     u32 flags,
				     u64 pfn_offset, u64 pfn_count,
				     int (*handler)(struct mshv_mem_region *region,
						    u32 flags,
						    u64 pfn_offset,
						    u64 pfn_count,
						    bool huge_page))
{
	u64 gfn = region->start_gfn + pfn_offset;
	u64 count;
	unsigned long pfn;
	int stride, ret;

	pfn = region->mreg_pfns[pfn_offset];
	if (!pfn_valid(pfn))
		return -EINVAL;

	stride = mshv_chunk_stride(pfn_to_page(pfn), gfn, pfn_count);
	if (stride < 0)
		return stride;

	/* Start at stride since the first stride is validated */
	for (count = stride; count < pfn_count ; count += stride) {
		pfn = region->mreg_pfns[pfn_offset + count];

		/* Break if current pfn is invalid */
		if (pfn != MSHV_INVALID_PFN)
			break;

		/* Break if stride size changes */
		if (stride != mshv_chunk_stride(pfn_to_page(pfn),
						gfn + count,
						pfn_count - count))
			break;
	}

	ret = handler(region, flags, pfn_offset, count, stride > 1);
	if (ret)
		return ret;

	return count;
}

/**
 * mshv_region_process_hole - Handle a hole (invalid PFNs) in a memory
 *                            region
 * @region    : Memory region containing the hole
 * @flags     : Flags to pass to the handler function
 * @pfn_offset: Starting PFN offset within the region
 * @pfn_count : Number of PFNs in the hole
 * @handler   : Callback function to invoke for the hole
 *
 * Invokes the handler function for a contiguous hole with the specified
 * parameters.
 *
 * Return: Number of PFNs handled, or negative error code.
 */
static long mshv_region_process_hole(struct mshv_mem_region *region,
				     u32 flags,
				     u64 pfn_offset, u64 pfn_count,
				     int (*handler)(struct mshv_mem_region *region,
						    u32 flags,
						    u64 pfn_offset,
						    u64 pfn_count,
						    bool huge_page))
{
	long ret;

	ret = handler(region, flags, pfn_offset, pfn_count, 0);
	if (ret)
		return ret;

	return pfn_count;
}

static long mshv_region_process_chunk(struct mshv_mem_region *region,
				      u32 flags,
				      u64 pfn_offset, u64 pfn_count,
				      int (*handler)(struct mshv_mem_region *region,
						     u32 flags,
						     u64 pfn_offset,
						     u64 pfn_count,
						     bool huge_page))
{
	if (pfn_valid(region->mreg_pfns[pfn_offset]))
		return mshv_region_process_pfns(region, flags,
				pfn_offset, pfn_count,
				handler);
	else
		return mshv_region_process_hole(region, flags,
				pfn_offset, pfn_count,
				handler);
}

/**
 * mshv_region_process_range - Processes a range of PFNs in a region.
 * @region    : Pointer to the memory region structure.
 * @flags     : Flags to pass to the handler.
 * @pfn_offset: Offset into the region's PFNs array to start processing.
 * @pfn_count : Number of PFNs to process.
 * @handler   : Callback function to handle each chunk of contiguous
 *              valid PFNs.
 *
 * Iterates over the specified range of PFNs in @region, skipping
 * invalid PFNs. For each contiguous chunk of valid PFNS, invokes
 * @handler via mshv_region_process_pfns.
 *
 * Note: The @handler callback must be able to handle PFNs backed by both
 * normal and huge pages.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
static int mshv_region_process_range(struct mshv_mem_region *region,
				     u32 flags,
				     u64 pfn_offset, u64 pfn_count,
				     int (*handler)(struct mshv_mem_region *region,
						    u32 flags,
						    u64 pfn_offset,
						    u64 pfn_count,
						    bool huge_page))
{
	u64 start, end;
	long ret;

	if (!pfn_count)
		return 0;

	if (check_add_overflow(pfn_offset, pfn_count, &end))
		return -EOVERFLOW;

	if (end > region->nr_pfns)
		return -EINVAL;

	start = pfn_offset;
	end = pfn_offset + 1;

	while (end < pfn_offset + pfn_count) {
		/*
		 * Accumulate contiguous pfns with the same validity
		 * (valid or not).
		 */
		if (pfn_valid(region->mreg_pfns[start]) ==
		    pfn_valid(region->mreg_pfns[end])) {
			end++;
			continue;
		}

		ret = mshv_region_process_chunk(region, flags,
						start, end - start,
						handler);
		if (ret < 0)
			return ret;

		start += ret;
	}

	ret = mshv_region_process_chunk(region, flags,
					start, end - start,
					handler);
	if (ret < 0)
		return ret;

	return 0;
}

struct mshv_mem_region *mshv_region_create(u64 guest_pfn, u64 nr_pfns,
					   u64 uaddr, u32 flags)
{
	struct mshv_mem_region *region;
	u64 i;

	region = vzalloc(struct_size(region, mreg_pfns, nr_pfns));
	if (!region)
		return ERR_PTR(-ENOMEM);

	region->nr_pfns = nr_pfns;
	region->start_gfn = guest_pfn;
	region->start_uaddr = uaddr;
	region->hv_map_flags = HV_MAP_GPA_READABLE | HV_MAP_GPA_ADJUSTABLE;
	if (flags & BIT(MSHV_SET_MEM_BIT_WRITABLE))
		region->hv_map_flags |= HV_MAP_GPA_WRITABLE;
	if (flags & BIT(MSHV_SET_MEM_BIT_EXECUTABLE))
		region->hv_map_flags |= HV_MAP_GPA_EXECUTABLE;

	for (i = 0; i < nr_pfns; i++)
		region->mreg_pfns[i] = MSHV_INVALID_PFN;

	kref_init(&region->mreg_refcount);

	return region;
}

static int mshv_region_chunk_share(struct mshv_mem_region *region,
				   u32 flags,
				   u64 pfn_offset, u64 pfn_count,
				   bool huge_page)
{
	if (!pfn_valid(region->mreg_pfns[pfn_offset]))
		return -EINVAL;

	if (huge_page)
		flags |= HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE;

	return hv_call_modify_spa_host_access(region->partition->pt_id,
					      region->mreg_pfns + pfn_offset,
					      pfn_count,
					      HV_MAP_GPA_READABLE |
					      HV_MAP_GPA_WRITABLE,
					      flags, true);
}

static int mshv_region_share(struct mshv_mem_region *region)
{
	u32 flags = HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_SHARED;

	return mshv_region_process_range(region, flags,
					 0, region->nr_pfns,
					 mshv_region_chunk_share);
}

static int mshv_region_chunk_unshare(struct mshv_mem_region *region,
				     u32 flags,
				     u64 pfn_offset, u64 pfn_count,
				     bool huge_page)
{
	if (!pfn_valid(region->mreg_pfns[pfn_offset]))
		return -EINVAL;

	if (huge_page)
		flags |= HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE;

	return hv_call_modify_spa_host_access(region->partition->pt_id,
					      region->mreg_pfns + pfn_offset,
					      pfn_count, 0,
					      flags, false);
}

static int mshv_region_unshare(struct mshv_mem_region *region)
{
	u32 flags = HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_EXCLUSIVE;

	return mshv_region_process_range(region, flags,
					 0, region->nr_pfns,
					 mshv_region_chunk_unshare);
}

static int mshv_region_chunk_remap(struct mshv_mem_region *region,
				   u32 flags,
				   u64 pfn_offset, u64 pfn_count,
				   bool huge_page)
{
	/*
	 * Remap missing pages with no access to let the
	 * hypervisor track dirty pages, enabling precopy live
	 * migration.
	 */
	if (!pfn_valid(region->mreg_pfns[pfn_offset]))
		flags = HV_MAP_GPA_NO_ACCESS;

	if (huge_page)
		flags |= HV_MAP_GPA_LARGE_PAGE;

	return hv_call_map_ram_pfns(region->partition->pt_id,
				    region->start_gfn + pfn_offset,
				    pfn_count, flags,
				    region->mreg_pfns + pfn_offset);
}

static int mshv_region_remap_pfns(struct mshv_mem_region *region,
				  u32 map_flags,
				  u64 pfn_offset, u64 pfn_count)
{
	return mshv_region_process_range(region, map_flags,
					 pfn_offset, pfn_count,
					 mshv_region_chunk_remap);
}

static int mshv_region_map(struct mshv_mem_region *region)
{
	u32 map_flags = region->hv_map_flags;

	return mshv_region_remap_pfns(region, map_flags,
				      0, region->nr_pfns);
}

static void mshv_region_invalidate_pfns(struct mshv_mem_region *region,
					u64 pfn_offset, u64 pfn_count)
{
	u64 i;

	for (i = pfn_offset; i < pfn_offset + pfn_count; i++) {
		if (!pfn_valid(region->mreg_pfns[i]))
			continue;

		if (region->mreg_type == MSHV_REGION_TYPE_MEM_PINNED)
			unpin_user_page(pfn_to_page(region->mreg_pfns[i]));

		region->mreg_pfns[i] = MSHV_INVALID_PFN;
	}
}

static void mshv_region_invalidate(struct mshv_mem_region *region)
{
	mshv_region_invalidate_pfns(region, 0, region->nr_pfns);
}

static int mshv_region_pin(struct mshv_mem_region *region)
{
	u64 done_count, nr_pfns, i;
	unsigned long *pfns;
	struct page **pages;
	__u64 userspace_addr;
	int ret;

	pages = kmalloc_array(MSHV_PIN_PAGES_BATCH_SIZE,
			      sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (done_count = 0; done_count < region->nr_pfns; done_count += ret) {
		pfns = region->mreg_pfns + done_count;
		userspace_addr = region->start_uaddr +
				 done_count * HV_HYP_PAGE_SIZE;
		nr_pfns = min(region->nr_pfns - done_count,
			      MSHV_PIN_PAGES_BATCH_SIZE);

		/*
		 * Pinning assuming 4k pages works for large pages too.
		 * All page structs within the large page are returned.
		 *
		 * Pin requests are batched because pin_user_pages_fast
		 * with the FOLL_LONGTERM flag does a large temporary
		 * allocation of contiguous memory.
		 */
		ret = pin_user_pages_fast(userspace_addr, nr_pfns,
					  FOLL_WRITE | FOLL_LONGTERM,
					  pages);
		if (ret != nr_pfns)
			goto release_pages;

		for (i = 0; i < ret; i++)
			pfns[i] = page_to_pfn(pages[i]);
	}

	kfree(pages);
	return 0;

release_pages:
	if (ret > 0)
		done_count += ret;
	mshv_region_invalidate_pfns(region, 0, done_count);
	kfree(pages);
	return ret < 0 ? ret : -ENOMEM;
}

static int mshv_region_chunk_unmap(struct mshv_mem_region *region,
				   u32 flags,
				   u64 pfn_offset, u64 pfn_count,
				   bool huge_page)
{
	if (!pfn_valid(region->mreg_pfns[pfn_offset]))
		return 0;

	if (huge_page)
		flags |= HV_UNMAP_GPA_LARGE_PAGE;

	return hv_call_unmap_pfns(region->partition->pt_id,
				  region->start_gfn + pfn_offset,
				  pfn_count, flags);
}

static int mshv_region_unmap(struct mshv_mem_region *region)
{
	return mshv_region_process_range(region, 0,
					 0, region->nr_pfns,
					 mshv_region_chunk_unmap);
}

static void mshv_region_destroy(struct kref *ref)
{
	struct mshv_mem_region *region =
		container_of(ref, struct mshv_mem_region, mreg_refcount);
	struct mshv_partition *partition = region->partition;
	int ret;

	if (region->mreg_type == MSHV_REGION_TYPE_MEM_MOVABLE)
		mshv_region_movable_fini(region);

	if (mshv_partition_encrypted(partition)) {
		ret = mshv_region_share(region);
		if (ret) {
			pt_err(partition,
			       "Failed to regain access to memory, unpinning user pages will fail and crash the host error: %d\n",
			       ret);
			return;
		}
	}

	mshv_region_unmap(region);

	mshv_region_invalidate(region);

	vfree(region);
}

void mshv_region_put(struct mshv_mem_region *region)
{
	kref_put(&region->mreg_refcount, mshv_region_destroy);
}

int mshv_region_get(struct mshv_mem_region *region)
{
	return kref_get_unless_zero(&region->mreg_refcount);
}

/**
 * mshv_region_hmm_fault_and_lock - Handle HMM faults across VMAs and lock
 *                                  the memory region
 * @region: Pointer to the memory region structure
 * @start : Starting virtual address of the range to fault
 * @end   : Ending virtual address of the range to fault (exclusive)
 * @pfns  : Output array for page frame numbers with HMM flags
 *
 * This function performs the following steps:
 * 1. Reads the notifier sequence for the HMM range.
 * 2. Acquires a read lock on the memory map.
 * 3. Iterates through VMAs in the specified range, handling each
 *    separately with appropriate protection flags (HMM_PFN_REQ_WRITE set
 *    based on VMA flags).
 * 4. Handles HMM faults for each VMA segment.
 * 5. Releases the read lock on the memory map.
 * 6. If successful, locks the memory region mutex.
 * 7. Verifies if the notifier sequence has changed during the operation.
 *    If it has, releases the mutex and returns -EBUSY to signal retry.
 *
 * The function expects the range [start, end) is backed by valid VMAs.
 * Returns -EFAULT if any address in the range is not covered by a VMA.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int mshv_region_hmm_fault_and_lock(struct mshv_mem_region *region,
					  unsigned long start,
					  unsigned long end,
					  unsigned long *pfns,
					  bool do_fault)
{
	struct hmm_range range = {
		.notifier = &region->mreg_mni,
	};
	struct mm_struct *mm = region->mreg_mni.mm;
	int ret;

	range.notifier_seq = mmu_interval_read_begin(range.notifier);
	mmap_read_lock(mm);
	while (start < end) {
		struct vm_area_struct *vma;

		vma = vma_lookup(mm, start);
		if (!vma) {
			ret = -EFAULT;
			break;
		}

		range.hmm_pfns = pfns;
		range.start = start;
		range.end = min(vma->vm_end, end);
		range.default_flags = 0;
		if (do_fault) {
			range.default_flags = HMM_PFN_REQ_FAULT;
			if (vma->vm_flags & VM_WRITE)
				range.default_flags |= HMM_PFN_REQ_WRITE;
		}

		ret = hmm_range_fault(&range);
		if (ret)
			break;

		start = range.end;
		pfns += (range.end - range.start) >> PAGE_SHIFT;
	}
	mmap_read_unlock(mm);
	if (ret)
		return ret;

	mutex_lock(&region->mreg_mutex);

	if (mmu_interval_read_retry(range.notifier, range.notifier_seq)) {
		mutex_unlock(&region->mreg_mutex);
		cond_resched();
		return -EBUSY;
	}

	return 0;
}

/**
 * mshv_region_collect_and_map - Collect PFNs for a user range and map them
 * @region    : memory region being processed
 * @pfn_offset: PFNs offset within the region
 * @pfn_count : number of PFNs to process
 * @do_fault  : if true, fault in missing pages;
 *              if false, collect only present pages
 *
 * Collects PFNs for the specified portion of @region from the
 * corresponding userspace VMAs and maps them into the hypervisor. The
 * behavior depends on @do_fault:
 *
 * - true: Fault in missing pages from userspace, ensuring all pages in the
 *   range are present. Used for on-demand page population.
 * - false: Collect PFNs only for pages already present in userspace,
 *   leaving missing pages as invalid PFN markers.
 *   Used for initial region setup.
 *
 * Collected PFNs are stored in region->mreg_pfns[] with HMM bookkeeping
 * flags cleared, then the range is mapped into the hypervisor. Present
 * PFNs get mapped with region access permissions; missing PFNs (invalid
 * entries) get mapped with no-access permissions.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int mshv_region_collect_and_map(struct mshv_mem_region *region,
				       u64 pfn_offset, u64 pfn_count,
				       bool do_fault)
{
	unsigned long start, end;
	unsigned long *pfns;
	int ret;
	u64 i;

	pfns = vmalloc_array(pfn_count, sizeof(unsigned long));
	if (!pfns)
		return -ENOMEM;

	start = region->start_uaddr + pfn_offset * PAGE_SIZE;
	end = start + pfn_count * PAGE_SIZE;

	do {
		ret = mshv_region_hmm_fault_and_lock(region, start, end,
						     pfns, do_fault);
	} while (ret == -EBUSY);

	if (ret)
		goto out;

	for (i = 0; i < pfn_count; i++) {
		if (!(pfns[i] & HMM_PFN_VALID))
			continue;
		/* Drop HMM_PFN_* flags to ensure PFNs are valid. */
		region->mreg_pfns[pfn_offset + i] = pfns[i] & ~HMM_PFN_FLAGS;
	}

	ret = mshv_region_remap_pfns(region, region->hv_map_flags,
				     pfn_offset, pfn_count);

	mutex_unlock(&region->mreg_mutex);
out:
	vfree(pfns);
	return ret;
}

static int mshv_region_range_fault(struct mshv_mem_region *region,
				   u64 pfn_offset, u64 pfn_count)
{
	return mshv_region_collect_and_map(region, pfn_offset, pfn_count,
					   true);
}

bool mshv_region_handle_gfn_fault(struct mshv_mem_region *region, u64 gfn)
{
	u64 pfn_offset, pfn_count;
	int ret;

	/* Align the page offset to the nearest MSHV_MAP_FAULT_IN_PAGES. */
	pfn_offset = ALIGN_DOWN(gfn - region->start_gfn,
				MSHV_MAP_FAULT_IN_PAGES);

	/* Map more pages than requested to reduce the number of faults. */
	pfn_count = min(region->nr_pfns - pfn_offset,
			MSHV_MAP_FAULT_IN_PAGES);

	ret = mshv_region_range_fault(region, pfn_offset, pfn_count);

	WARN_ONCE(ret,
		  "p%llu: GPA intercept failed: region %#llx-%#llx, gfn %#llx, pfn_offset %llu, pfn_count %llu\n",
		  region->partition->pt_id, region->start_uaddr,
		  region->start_uaddr + (region->nr_pfns << HV_HYP_PAGE_SHIFT),
		  gfn, pfn_offset, pfn_count);

	return !ret;
}

/**
 * mshv_region_interval_invalidate - Invalidate a range of memory region
 * @mni: Pointer to the mmu_interval_notifier structure
 * @range: Pointer to the mmu_notifier_range structure
 * @cur_seq: Current sequence number for the interval notifier
 *
 * This function invalidates a memory region by remapping its pages with
 * no access permissions. It locks the region's mutex to ensure thread safety
 * and updates the sequence number for the interval notifier. If the range
 * is blockable, it uses a blocking lock; otherwise, it attempts a non-blocking
 * lock and returns false if unsuccessful.
 *
 * NOTE: Failure to invalidate a region is a serious error, as the pages will
 * be considered freed while they are still mapped by the hypervisor.
 * Any attempt to access such pages will likely crash the system.
 *
 * Return: true if the region was successfully invalidated, false otherwise.
 */
static bool mshv_region_interval_invalidate(struct mmu_interval_notifier *mni,
					    const struct mmu_notifier_range *range,
					    unsigned long cur_seq)
{
	struct mshv_mem_region *region = container_of(mni,
						      struct mshv_mem_region,
						      mreg_mni);
	u64 pfn_offset, pfn_count;
	unsigned long mstart, mend;
	int ret = -EPERM;

	mstart = max(range->start, region->start_uaddr);
	mend = min(range->end, region->start_uaddr +
		   (region->nr_pfns << HV_HYP_PAGE_SHIFT));

	pfn_offset = HVPFN_DOWN(mstart - region->start_uaddr);
	pfn_count = HVPFN_DOWN(mend - mstart);

	if (mmu_notifier_range_blockable(range))
		mutex_lock(&region->mreg_mutex);
	else if (!mutex_trylock(&region->mreg_mutex))
		goto out_fail;

	mmu_interval_set_seq(mni, cur_seq);

	ret = mshv_region_remap_pfns(region, HV_MAP_GPA_NO_ACCESS,
				     pfn_offset, pfn_count);
	if (ret)
		goto out_unlock;

	mshv_region_invalidate_pfns(region, pfn_offset, pfn_count);

	mutex_unlock(&region->mreg_mutex);

	return true;

out_unlock:
	mutex_unlock(&region->mreg_mutex);
out_fail:
	WARN_ONCE(ret,
		  "Failed to invalidate region %#llx-%#llx (range %#lx-%#lx, event: %u, pages %#llx-%#llx, mm: %#llx): %d\n",
		  region->start_uaddr,
		  region->start_uaddr + (region->nr_pfns << HV_HYP_PAGE_SHIFT),
		  range->start, range->end, range->event,
		  pfn_offset, pfn_offset + pfn_count - 1, (u64)range->mm, ret);
	return false;
}

static const struct mmu_interval_notifier_ops mshv_region_mni_ops = {
	.invalidate = mshv_region_interval_invalidate,
};

void mshv_region_movable_fini(struct mshv_mem_region *region)
{
	mmu_interval_notifier_remove(&region->mreg_mni);
}

bool mshv_region_movable_init(struct mshv_mem_region *region)
{
	int ret;

	ret = mmu_interval_notifier_insert(&region->mreg_mni, current->mm,
					   region->start_uaddr,
					   region->nr_pfns << HV_HYP_PAGE_SHIFT,
					   &mshv_region_mni_ops);
	if (ret)
		return false;

	mutex_init(&region->mreg_mutex);

	return true;
}

/**
 * mshv_map_pinned_region - Pin and map memory regions
 * @region: Pointer to the memory region structure
 *
 * This function processes memory regions that are explicitly marked as pinned.
 * Pinned regions are preallocated, mapped upfront, and do not rely on fault-based
 * population. The function ensures the region is properly populated, handles
 * encryption requirements for SNP partitions if applicable, maps the region,
 * and performs necessary sharing or eviction operations based on the mapping
 * result.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mshv_map_pinned_region(struct mshv_mem_region *region)
{
	struct mshv_partition *partition = region->partition;
	int ret;

	ret = mshv_region_pin(region);
	if (ret) {
		pt_err(partition, "Failed to pin memory region: %d\n",
		       ret);
		goto err_out;
	}

	/*
	 * For an SNP partition it is a requirement that for every memory region
	 * that we are going to map for this partition we should make sure that
	 * host access to that region is released. This is ensured by doing an
	 * additional hypercall which will update the SLAT to release host
	 * access to guest memory regions.
	 */
	if (mshv_partition_encrypted(partition)) {
		ret = mshv_region_unshare(region);
		if (ret) {
			pt_err(partition,
			       "Failed to unshare memory region (guest_pfn: %llu): %d\n",
			       region->start_gfn, ret);
			goto invalidate_region;
		}
	}

	ret = mshv_region_map(region);
	if (!ret)
		return 0;

	if (mshv_partition_encrypted(partition)) {
		int shrc;

		shrc = mshv_region_share(region);
		if (!shrc)
			goto invalidate_region;

		pt_err(partition,
		       "Failed to share memory region (guest_pfn: %llu): %d\n",
		       region->start_gfn, shrc);
		/*
		 * Don't unpin if marking shared failed because pages are no
		 * longer mapped in the host, ie root, anymore.
		 */
		goto err_out;
	}

invalidate_region:
	mshv_region_invalidate(region);
err_out:
	return ret;
}

int mshv_map_movable_region(struct mshv_mem_region *region)
{
	return mshv_region_collect_and_map(region, 0, region->nr_pfns,
					   false);
}
