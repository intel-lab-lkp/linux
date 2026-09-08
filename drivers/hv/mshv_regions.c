// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Memory region management for mshv_root module.
 *
 * Authors: Microsoft Linux virtualization team
 */

#include <linux/bitmap.h>
#include <linux/hmm.h>
#include <linux/hyperv.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>

#include <asm/mshyperv.h>

#include "mshv_root.h"

#define MSHV_MAP_FAULT_IN_PAGES				PTRS_PER_PMD

/**
 * mshv_chunk_stride - Compute stride for mapping guest memory
 * @page      : The page to check for huge page backing
 * @gfn       : Guest frame number for the mapping
 * @page_count: Total number of pages in the mapping
 *
 * Determines the appropriate stride (in pages) for mapping guest memory.
 * Uses huge page stride if the backing page is huge and the guest mapping
 * is properly aligned; otherwise falls back to single page stride.
 *
 * Return: Stride in pages.
 */
static unsigned int mshv_chunk_stride(struct page *page, u64 gfn,
				      u64 page_count)
{
	unsigned int page_order = folio_order(page_folio(page));

	/*
	 * Use single page stride by default. For huge page stride, the
	 * folio order must be at least PMD_ORDER, the page's PFN must be
	 * 2M-aligned (so that a 2M-aligned tail page of a larger folio is
	 * acceptable), and both gfn and page_count must be 2M-aligned.
	 */
	if (page_order < PMD_ORDER ||
	    !IS_ALIGNED(page_to_pfn(page), PTRS_PER_PMD) ||
	    !IS_ALIGNED(gfn, PTRS_PER_PMD) ||
	    !IS_ALIGNED(page_count, PTRS_PER_PMD))
		return 1;

	/* Use 2M stride always i.e. process 1G folios as 2M chunks */
	return 1 << PMD_ORDER;
}

/**
 * mshv_region_process_chunk - Processes a contiguous chunk of memory pages
 *                             in a region.
 * @region     : Pointer to the memory region structure.
 * @flags      : Flags to pass to the handler.
 * @page_offset: Offset into the region's pages array to start processing.
 * @page_count : Number of pages to process.
 * @handler    : Callback function to handle the chunk.
 *
 * This function scans the region's pages starting from @page_offset,
 * checking for contiguous present pages of the same size (normal or huge).
 * It invokes @handler for the chunk of contiguous pages found. Returns the
 * number of pages handled, or a negative error code if the first page is
 * not present or the handler fails.
 *
 * Note: The @handler callback must be able to handle both normal and huge
 * pages.
 *
 * Return: Number of pages handled, or negative error code.
 */
static long mshv_region_process_chunk(struct mshv_mem_region *region,
				      u32 flags,
				      u64 page_offset, u64 page_count,
				      int (*handler)(struct mshv_mem_region *region,
						     u32 flags,
						     u64 page_offset,
						     u64 page_count,
						     bool huge_page))
{
	u64 gfn = region->start_gfn + page_offset;
	u64 count;
	struct page *page;
	unsigned int stride;
	int ret;

	page = region->mreg_pages[page_offset];
	if (!page)
		return -EINVAL;

	stride = mshv_chunk_stride(page, gfn, page_count);

	/* Start at stride since the first stride is validated */
	for (count = stride; count < page_count; count += stride) {
		page = region->mreg_pages[page_offset + count];

		/* Break if current page is not present */
		if (!page)
			break;

		/* Break if stride size changes */
		if (stride != mshv_chunk_stride(page, gfn + count,
						page_count - count))
			break;
	}

	ret = handler(region, flags, page_offset, count, stride > 1);
	if (ret)
		return ret;

	return count;
}

/**
 * mshv_region_process_range - Processes a range of memory pages in a
 *                             region.
 * @region     : Pointer to the memory region structure.
 * @flags      : Flags to pass to the handler.
 * @page_offset: Offset into the region's pages array to start processing.
 * @page_count : Number of pages to process.
 * @handler    : Callback function to handle each chunk of contiguous
 *               pages.
 *
 * Iterates over the specified range of pages in @region, skipping
 * non-present pages. For each contiguous chunk of present pages, invokes
 * @handler via mshv_region_process_chunk.
 *
 * Note: The @handler callback must be able to handle both normal and huge
 * pages.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
static int mshv_region_process_range(struct mshv_mem_region *region,
				     u32 flags,
				     u64 page_offset, u64 page_count,
				     int (*handler)(struct mshv_mem_region *region,
						    u32 flags,
						    u64 page_offset,
						    u64 page_count,
						    bool huge_page))
{
	long ret;

	if (page_offset + page_count > region->nr_pages)
		return -EINVAL;

	while (page_count) {
		/* Skip non-present pages */
		if (!region->mreg_pages[page_offset]) {
			page_offset++;
			page_count--;
			continue;
		}

		ret = mshv_region_process_chunk(region, flags,
						page_offset,
						page_count,
						handler);
		if (ret < 0)
			return ret;

		page_offset += ret;
		page_count -= ret;
	}

	return 0;
}

struct mshv_mem_region *mshv_region_create(u64 guest_pfn, u64 nr_pages,
					   u64 uaddr, u32 flags)
{
	struct mshv_mem_region *region;

	region = vzalloc(sizeof(*region) + sizeof(struct page *) * nr_pages);
	if (!region)
		return ERR_PTR(-ENOMEM);

	region->nr_pages = nr_pages;
	region->start_gfn = guest_pfn;
	region->start_uaddr = uaddr;
	region->hv_map_flags = HV_MAP_GPA_READABLE | HV_MAP_GPA_ADJUSTABLE;
	if (flags & BIT(MSHV_SET_MEM_BIT_WRITABLE))
		region->hv_map_flags |= HV_MAP_GPA_WRITABLE;
	if (flags & BIT(MSHV_SET_MEM_BIT_EXECUTABLE))
		region->hv_map_flags |= HV_MAP_GPA_EXECUTABLE;

	kref_init(&region->mreg_refcount);
	mutex_init(&region->mreg_mutex);
	init_rwsem(&region->mreg_fault_lock);

	return region;
}

int mshv_region_host_access_init(struct mshv_mem_region *region)
{
	unsigned int nr_pages;
	unsigned int longs;
	size_t bytes;

	if (region->nr_pages > UINT_MAX)
		return -E2BIG;

	nr_pages = region->nr_pages;
	longs = BITS_TO_LONGS(nr_pages);
	bytes = array_size(longs, sizeof(unsigned long));
	region->host_access_read = kvcalloc(4, bytes, GFP_KERNEL);
	if (!region->host_access_read)
		return -ENOMEM;

	region->host_access_write = region->host_access_read + longs;
	region->host_access_exclusive = region->host_access_write + longs;
	region->host_access_uncertain =
		region->host_access_exclusive + longs;
	bitmap_fill(region->host_access_read, nr_pages);
	bitmap_fill(region->host_access_write, nr_pages);
	return 0;
}

int mshv_region_check_duplicate_pfns(struct mshv_mem_region *region)
{
	struct mshv_partition *partition = region->partition;
	struct mshv_mem_region *existing;
	DEFINE_XARRAY(pfns);
	u64 i;
	int ret = 0;

	lockdep_assert_held(&partition->pt_mutex);

	/*
	 * Use a temporary index so alias rejection has no permanent per-page
	 * memory cost. xa_insert() also rejects duplicate PFNs within region.
	 */
	for (i = 0; i < region->nr_pages; i++) {
		ret = xa_insert(&pfns, page_to_pfn(region->mreg_pages[i]),
				region, GFP_KERNEL);
		if (ret) {
			if (ret == -EBUSY)
				ret = -EEXIST;
			goto out;
		}
	}

	hlist_for_each_entry(existing, &partition->pt_mem_regions, hnode) {
		if (existing->mreg_type != MSHV_REGION_TYPE_MEM_PINNED)
			continue;
		for (i = 0; i < existing->nr_pages; i++) {
			if (!xa_load(&pfns,
				     page_to_pfn(existing->mreg_pages[i])))
				continue;
			ret = -EEXIST;
			goto out;
		}
	}

out:
	xa_destroy(&pfns);
	return ret;
}

static bool mshv_host_access_state_is_baseline(u8 state)
{
	return state == MSHV_HOST_ACCESS_BASELINE;
}

u8 mshv_region_host_access_get(const struct mshv_mem_region *region, u64 index)
{
	u8 state = 0;

	if (WARN_ON_ONCE(!region->host_access_read ||
			 index >= region->nr_pages))
		return MSHV_HOST_ACCESS_UNCERTAIN;

	if (test_bit(index, region->host_access_read))
		state |= MSHV_HOST_ACCESS_READ;
	if (test_bit(index, region->host_access_write))
		state |= MSHV_HOST_ACCESS_WRITE;
	if (test_bit(index, region->host_access_exclusive))
		state |= MSHV_HOST_ACCESS_EXCLUSIVE;
	if (test_bit(index, region->host_access_uncertain))
		state |= MSHV_HOST_ACCESS_UNCERTAIN;
	return state;
}

void mshv_region_host_access_set(struct mshv_mem_region *region, u64 index,
				 u8 state)
{
	u8 old = mshv_region_host_access_get(region, index);

	if (mshv_host_access_state_is_baseline(old) &&
	    !mshv_host_access_state_is_baseline(state))
		region->host_nonbaseline_pages++;
	else if (!mshv_host_access_state_is_baseline(old) &&
		 mshv_host_access_state_is_baseline(state))
		region->host_nonbaseline_pages--;

	assign_bit(index, region->host_access_read,
		   state & MSHV_HOST_ACCESS_READ);
	assign_bit(index, region->host_access_write,
		   state & MSHV_HOST_ACCESS_WRITE);
	assign_bit(index, region->host_access_exclusive,
		   state & MSHV_HOST_ACCESS_EXCLUSIVE);
	assign_bit(index, region->host_access_uncertain,
		   state & MSHV_HOST_ACCESS_UNCERTAIN);
}

static void
mshv_region_host_access_mark_uncertain(struct mshv_mem_region *region,
				       u64 index, u64 count)
{
	u64 i;

	for (i = 0; i < count; i++) {
		u8 state = mshv_region_host_access_get(region, index + i);

		mshv_region_host_access_set(region, index + i, state | MSHV_HOST_ACCESS_UNCERTAIN);
	}
}

static int
mshv_host_access_normalize_result(struct mshv_host_access_result *result,
				  u64 count)
{
	if (result->completed > count) {
		result->completed = 0;
		result->uncertain = true;
		result->error = -EPROTO;
	} else if (!result->error && result->completed != count) {
		result->uncertain = true;
		result->error = -EPROTO;
	}

	return result->error;
}

u8 mshv_make_host_access_state(u8 old_state, u32 host_access, u32 flags)
{
	/* Permissions are replaced; only explicit flags change ownership. */
	u8 state = old_state & (MSHV_HOST_ACCESS_EXCLUSIVE |
			      MSHV_HOST_ACCESS_UNCERTAIN);

	if (host_access & HV_MAP_GPA_READABLE)
		state |= MSHV_HOST_ACCESS_READ;
	if (host_access & HV_MAP_GPA_WRITABLE)
		state |= MSHV_HOST_ACCESS_WRITE;

	if (flags & HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_EXCLUSIVE) {
		state |= MSHV_HOST_ACCESS_EXCLUSIVE;
		state &= ~MSHV_HOST_ACCESS_UNCERTAIN;
	} else if (flags & HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_SHARED) {
		state &= ~(MSHV_HOST_ACCESS_EXCLUSIVE |
			   MSHV_HOST_ACCESS_UNCERTAIN);
	}

	return state;
}

int
mshv_apply_region_host_access(struct mshv_mem_region *region, u64 index,
			      u64 count, u32 host_access, u32 flags,
			      struct mshv_host_access_result *result)
{
	u64 i;

	mshv_host_access_normalize_result(result, count);
	for (i = 0; i < result->completed; i++) {
		u8 old_state = mshv_region_host_access_get(region, index + i);
		u8 state = mshv_make_host_access_state(old_state, host_access,
						       flags);

		mshv_region_host_access_set(region, index + i, state);
	}
	if (result->uncertain) {
		u64 remaining = count - result->completed;
		u64 uncertain_index = index + result->completed;

		mshv_region_host_access_mark_uncertain(region, uncertain_index,
						       remaining);
	}

	return result->error;
}

static u8 mshv_host_access_page_state(const struct mshv_host_access_page *page)
{
	return mshv_region_host_access_get(page->region, page->index);
}

static void mshv_host_access_page_set(struct mshv_host_access_page *page,
				      u8 state)
{
	mshv_region_host_access_set(page->region, page->index, state);
}

int
mshv_apply_host_access(struct mshv_host_access_page *host_pages,
		       u64 count, u32 host_access, u32 flags,
		       struct mshv_host_access_result *result)
{
	u64 i;

	mshv_host_access_normalize_result(result, count);
	for (i = 0; i < result->completed; i++) {
		struct mshv_host_access_page *host_page = &host_pages[i];
		u8 old_state = mshv_host_access_page_state(host_page);
		u8 state = mshv_make_host_access_state(old_state, host_access,
						       flags);

		mshv_host_access_page_set(host_page, state);
	}
	if (result->uncertain) {
		for (i = result->completed; i < count; i++) {
			struct mshv_host_access_page *host_page = &host_pages[i];
			u8 old = mshv_host_access_page_state(host_page);

			mshv_host_access_page_set(host_page,
						  old | MSHV_HOST_ACCESS_UNCERTAIN);
		}
	}

	return result->error;
}

bool mshv_region_host_access_is_baseline(const struct mshv_mem_region *region)
{
	return !region->host_access_read || !region->host_nonbaseline_pages;
}

static int
mshv_region_modify_host_access(struct mshv_mem_region *region, u32 flags,
			       u64 page_offset, u64 page_count, bool huge_page,
			       u32 host_access, bool acquire)
{
	struct mshv_host_access_result result;

	if (huge_page)
		flags |= HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE;

	mshv_modify_host_access(region->partition->pt_id,
				region->mreg_pages + page_offset, page_count,
				host_access, flags, acquire, &result);

	/*
	 * completed is always a trusted count in 4K pages, including for 2M
	 * entries. The result helper is intentionally hypercall-independent so
	 * KUnit can inject partial and impossible completions.
	 */
	return mshv_apply_region_host_access(region, page_offset, page_count,
					     host_access, flags, &result);
}

static int mshv_region_chunk_share(struct mshv_mem_region *region,
				   u32 flags, u64 page_offset,
				   u64 page_count, bool huge_page)
{
	return mshv_region_modify_host_access(region, flags, page_offset,
					      page_count, huge_page,
					      HV_MAP_GPA_READABLE |
					      HV_MAP_GPA_WRITABLE, true);
}

int mshv_region_share(struct mshv_mem_region *region)
{
	u64 offset = 0;
	int state_error = 0;
	int ret;

	while (offset < region->nr_pages) {
		u64 count;
		u32 flags;
		u8 state = mshv_region_host_access_get(region, offset);

		if (state & MSHV_HOST_ACCESS_UNCERTAIN) {
			state_error = -EUCLEAN;
			offset++;
			continue;
		}
		if (mshv_host_access_state_is_baseline(state)) {
			offset++;
			continue;
		}

		flags = state & MSHV_HOST_ACCESS_EXCLUSIVE ?
			HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_SHARED : 0;
		/* One call may cover only pages with identical restore state. */
		for (count = 1; offset + count < region->nr_pages; count++) {
			u8 next = mshv_region_host_access_get(region,
							      offset + count);

			if (next != state)
				break;
		}

		ret = mshv_region_process_range(region, flags, offset, count,
						mshv_region_chunk_share);
		if (ret)
			return ret;
		offset += count;
	}
	return state_error;
}

static int mshv_region_chunk_unshare(struct mshv_mem_region *region,
				     u32 flags, u64 page_offset,
				     u64 page_count, bool huge_page)
{
	return mshv_region_modify_host_access(region, flags, page_offset,
					      page_count, huge_page, 0, false);
}

int mshv_region_unshare(struct mshv_mem_region *region)
{
	u32 flags = HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_EXCLUSIVE;

	return mshv_region_process_range(region, flags, 0, region->nr_pages,
					 mshv_region_chunk_unshare);
}

static int mshv_region_chunk_remap(struct mshv_mem_region *region,
				   u32 flags,
				   u64 page_offset, u64 page_count,
				   bool huge_page)
{
	bool chunk_mapping_may_exist;
	int ret;

	if (huge_page)
		flags |= HV_MAP_GPA_LARGE_PAGE;

	ret = hv_call_map_gpa_pages(region->partition->pt_id,
				    region->start_gfn + page_offset,
				    page_count, flags,
				    region->mreg_pages + page_offset,
				    &chunk_mapping_may_exist);
	if (chunk_mapping_may_exist)
		region->mapping_may_exist = true;

	return ret;
}

static int mshv_region_remap_pages(struct mshv_mem_region *region,
				   u32 map_flags,
				   u64 page_offset, u64 page_count)
{
	return mshv_region_process_range(region, map_flags,
					 page_offset, page_count,
					 mshv_region_chunk_remap);
}

int mshv_region_map(struct mshv_mem_region *region)
{
	u32 map_flags = region->hv_map_flags;

	return mshv_region_remap_pages(region, map_flags,
				       0, region->nr_pages);
}

static void mshv_region_invalidate_pages(struct mshv_mem_region *region,
					 u64 page_offset, u64 page_count)
{
	if (region->mreg_type == MSHV_REGION_TYPE_MEM_PINNED)
		unpin_user_pages(region->mreg_pages + page_offset, page_count);

	memset(region->mreg_pages + page_offset, 0,
	       page_count * sizeof(struct page *));
}

void mshv_region_invalidate(struct mshv_mem_region *region)
{
	mshv_region_invalidate_pages(region, 0, region->nr_pages);
}

int mshv_region_pin(struct mshv_mem_region *region)
{
	u64 done_count, nr_pages;
	struct page **pages;
	__u64 userspace_addr;
	int ret;

	for (done_count = 0; done_count < region->nr_pages; done_count += ret) {
		pages = region->mreg_pages + done_count;
		userspace_addr = region->start_uaddr +
				 done_count * HV_HYP_PAGE_SIZE;
		nr_pages = min(region->nr_pages - done_count,
			       MSHV_PIN_PAGES_BATCH_SIZE);

		/*
		 * Pinning assuming 4k pages works for large pages too.
		 * All page structs within the large page are returned.
		 *
		 * Pin requests are batched because pin_user_pages_fast
		 * with the FOLL_LONGTERM flag does a large temporary
		 * allocation of contiguous memory.
		 */
		ret = pin_user_pages_fast(userspace_addr, nr_pages,
					  FOLL_WRITE | FOLL_LONGTERM,
					  pages);
		if (ret != nr_pages)
			goto release_pages;
	}

	return 0;

release_pages:
	if (ret > 0)
		done_count += ret;
	mshv_region_invalidate_pages(region, 0, done_count);
	return ret < 0 ? ret : -ENOMEM;
}

static int mshv_region_chunk_unmap(struct mshv_mem_region *region,
				   u32 flags,
				   u64 page_offset, u64 page_count,
				   bool huge_page)
{
	if (huge_page)
		flags |= HV_UNMAP_GPA_LARGE_PAGE;

	return hv_call_unmap_gpa_pages(region->partition->pt_id,
				       region->start_gfn + page_offset,
				       page_count, flags);
}

int mshv_region_unmap(struct mshv_mem_region *region)
{
	int ret;

	if (!region->mapping_may_exist)
		return 0;

	if (region->mreg_type == MSHV_REGION_TYPE_MEM_PINNED)
		ret = mshv_region_process_range(region, 0, 0, region->nr_pages,
						mshv_region_chunk_unmap);
	else
		ret = hv_call_unmap_gpa_pages(region->partition->pt_id,
					      region->start_gfn,
					      region->nr_pages, 0);
	if (!ret)
		region->mapping_may_exist = false;

	return ret;
}

static void mshv_region_destroy(struct kref *ref)
{
	struct mshv_mem_region *region =
		container_of(ref, struct mshv_mem_region, mreg_refcount);

	/* Callers must retain the list reference until cleanup is certain. */
	if (WARN_ON_ONCE(region->mapping_may_exist ||
			 !mshv_region_host_access_is_baseline(region)))
		return;

	if (region->mreg_type == MSHV_REGION_TYPE_MEM_MOVABLE)
		mshv_region_movable_fini(region);

	kvfree(region->host_access_read);
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
 * mshv_region_range_fault - Fault and map pages for a movable region.
 * @region: Memory region receiving the GPA fault.
 * @page_offset: First page in the region.
 * @page_count: Number of pages to fault.
 *
 * mreg_fault_lock is only an admission and lifetime gate. A teardown writer
 * sets faults_blocked before taking the write side, which prevents new faults
 * and drains faults that can still remap the region. On an HMM retry, drop the
 * gate and mmap_lock before trying again so a waiting writer cannot create a
 * fairness cycle. The notifier never takes mreg_fault_lock.
 *
 * Lock order for one attempt is mreg_fault_lock -> mmap_lock -> mreg_mutex.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mshv_region_range_fault(struct mshv_mem_region *region,
				   u64 page_offset, u64 page_count)
{
	struct hmm_range range = {
		.notifier = &region->mreg_mni,
		.default_flags = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE,
	};
	struct mm_struct *mm;
	unsigned long *pfns;
	int ret;
	u64 i;

	pfns = kmalloc_array(page_count, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;

	range.hmm_pfns = pfns;
	range.start = region->start_uaddr + page_offset * HV_HYP_PAGE_SIZE;
	range.end = range.start + page_count * HV_HYP_PAGE_SIZE;

retry:
	down_read(&region->mreg_fault_lock);
	if (READ_ONCE(region->faults_blocked) ||
	    !mmget_not_zero(region->mreg_mni.mm)) {
		ret = -ECANCELED;
		goto out_unlock_faults;
	}
	mm = region->mreg_mni.mm;

	mmap_read_lock(mm);
	range.notifier_seq = mmu_interval_read_begin(range.notifier);
	ret = hmm_range_fault(&range);
	if (ret)
		goto out_unlock_mmap;

	mutex_lock(&region->mreg_mutex);
	if (mmu_interval_read_retry(range.notifier, range.notifier_seq)) {
		mutex_unlock(&region->mreg_mutex);
		ret = -EBUSY;
		goto out_unlock_mmap;
	}

	for (i = 0; i < page_count; i++)
		region->mreg_pages[page_offset + i] = hmm_pfn_to_page(pfns[i]);

	ret = mshv_region_remap_pages(region, region->hv_map_flags,
				      page_offset, page_count);
	mutex_unlock(&region->mreg_mutex);

out_unlock_mmap:
	mmap_read_unlock(mm);
	mmput(mm);
out_unlock_faults:
	up_read(&region->mreg_fault_lock);
	if (ret == -EBUSY) {
		cond_resched();
		goto retry;
	}
	kfree(pfns);
	return ret;
}

bool mshv_region_handle_gfn_fault(struct mshv_mem_region *region, u64 gfn)
{
	u64 page_offset, page_count;
	int ret;

	/* Align the page offset to the nearest MSHV_MAP_FAULT_IN_PAGES. */
	page_offset = ALIGN_DOWN(gfn - region->start_gfn,
				 MSHV_MAP_FAULT_IN_PAGES);

	/* Map more pages than requested to reduce the number of faults. */
	page_count = min(region->nr_pages - page_offset,
			 MSHV_MAP_FAULT_IN_PAGES);

	ret = mshv_region_range_fault(region, page_offset, page_count);

	WARN_ONCE(ret && ret != -ECANCELED,
		  "p%llu: GPA intercept failed: region %#llx-%#llx, gfn %#llx, page_offset %llu, page_count %llu\n",
		  region->partition->pt_id, region->start_uaddr,
		  region->start_uaddr +
		  (region->nr_pages << HV_HYP_PAGE_SHIFT),
		  gfn, page_offset, page_count);

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
	u64 page_offset, page_count;
	unsigned long mstart, mend;
	int ret = -EPERM;

	mstart = max(range->start, region->start_uaddr);
	mend = min(range->end, region->start_uaddr +
		   (region->nr_pages << HV_HYP_PAGE_SHIFT));

	page_offset = HVPFN_DOWN(mstart - region->start_uaddr);
	page_count = HVPFN_DOWN(mend - mstart);

	/*
	 * mmap_lock is already held by the notifier core. Take only the region
	 * mutex here; taking the fair fault rwsem would deadlock behind an unmap
	 * writer that is waiting for this mutex. Publish the sequence after the
	 * mutex is acquired even when a checked unmap proved no mapping remains.
	 */
	if (mmu_notifier_range_blockable(range))
		mutex_lock(&region->mreg_mutex);
	else if (!mutex_trylock(&region->mreg_mutex))
		goto out_fail;

	mmu_interval_set_seq(mni, cur_seq);

	if (READ_ONCE(region->mapping_may_exist)) {
		ret = mshv_region_remap_pages(region, HV_MAP_GPA_NO_ACCESS,
					      page_offset, page_count);
		if (ret)
			goto out_unlock;
	}

	mshv_region_invalidate_pages(region, page_offset, page_count);
	mutex_unlock(&region->mreg_mutex);
	return true;

out_unlock:
	mutex_unlock(&region->mreg_mutex);
out_fail:
	WARN_ONCE(ret,
		  "Failed to invalidate region %#llx-%#llx (range %#lx-%#lx, event: %u, pages %#llx-%#llx, mm: %#llx): %d\n",
		  region->start_uaddr,
		  region->start_uaddr + (region->nr_pages << HV_HYP_PAGE_SHIFT),
		  range->start, range->end, range->event,
		  page_offset, page_offset + page_count - 1, (u64)range->mm, ret);
	return false;
}

static const struct mmu_interval_notifier_ops mshv_region_mni_ops = {
	.invalidate = mshv_region_interval_invalidate,
};

void mshv_region_movable_fini(struct mshv_mem_region *region)
{
	if (!region->mni_registered)
		return;

	mmu_interval_notifier_remove(&region->mreg_mni);
	region->mni_registered = false;
}

bool mshv_region_movable_init(struct mshv_mem_region *region)
{
	int ret;

	ret = mmu_interval_notifier_insert(&region->mreg_mni, current->mm,
					   region->start_uaddr,
					   region->nr_pages << HV_HYP_PAGE_SHIFT,
					   &mshv_region_mni_ops);
	if (ret)
		return false;

	region->mni_registered = true;
	return true;
}
