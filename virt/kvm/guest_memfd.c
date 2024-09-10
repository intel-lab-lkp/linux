// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/backing-dev.h>
#include <linux/falloc.h>
#include <linux/hugetlb.h>
#include <linux/kvm_host.h>
#include <linux/pseudo_fs.h>
#include <linux/pagemap.h>
#include <linux/anon_inodes.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>

#include "kvm_mm.h"

static struct vfsmount *kvm_gmem_mnt;

struct kvm_gmem {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

struct kvm_gmem_hugetlb {
	struct hstate *h;
	struct hugepage_subpool *spool;
};

struct kvm_gmem_inode_private {
	struct xarray faultability;
	struct kvm_gmem_hugetlb *hgmem;
};

static struct kvm_gmem_inode_private *kvm_gmem_private(struct inode *inode)
{
	return inode->i_mapping->i_private_data;
}

static struct kvm_gmem_hugetlb *kvm_gmem_hgmem(struct inode *inode)
{
	return kvm_gmem_private(inode)->hgmem;
}

static bool is_kvm_gmem_hugetlb(struct inode *inode)
{
	u64 flags = (u64)inode->i_private;

	return flags & KVM_GUEST_MEMFD_HUGETLB;
}

#define KVM_GMEM_FAULTABILITY_VALUE 0x4641554c54  /* FAULT */

/**
 * Set faultability of given range of inode indices [@start, @end) to
 * @faultable. Return 0 if attributes were successfully updated or negative
 * errno on error.
 */
static int kvm_gmem_set_faultable(struct inode *inode, pgoff_t start, pgoff_t end,
				  bool faultable)
{
	struct xarray *faultability;
	void *val;
	pgoff_t i;

	/*
	 * The expectation is that fewer pages are faultable, hence save memory
	 * entries are created for faultable pages as opposed to creating
	 * entries for non-faultable pages.
	 */
	val = faultable ? xa_mk_value(KVM_GMEM_FAULTABILITY_VALUE) : NULL;
	faultability = &kvm_gmem_private(inode)->faultability;

	/*
	 * TODO replace this with something else (maybe interval
	 * tree?). store_range doesn't quite do what we expect if overlapping
	 * ranges are specified: if we store_range(5, 10, val) and then
	 * store_range(7, 12, NULL), the entire range [5, 12] will be NULL.  For
	 * now, use the slower xa_store() to store individual entries on indices
	 * to avoid this.
	 */
	for (i = start; i < end; i++) {
		int r;

		r = xa_err(xa_store(faultability, i, val, GFP_KERNEL_ACCOUNT));
		if (r)
			return r;
	}

	return 0;
}

/**
 * Return true if the page at @index is allowed to be faulted in.
 */
static bool kvm_gmem_is_faultable(struct inode *inode, pgoff_t index)
{
	struct xarray *faultability = &kvm_gmem_private(inode)->faultability;

	return xa_to_value(xa_load(faultability, index)) == KVM_GMEM_FAULTABILITY_VALUE;
}

/**
 * folio_file_pfn - like folio_file_page, but return a pfn.
 * @folio: The folio which contains this index.
 * @index: The index we want to look up.
 *
 * Return: The pfn for this index.
 */
static inline kvm_pfn_t folio_file_pfn(struct folio *folio, pgoff_t index)
{
	return folio_pfn(folio) + (index & (folio_nr_pages(folio) - 1));
}

static int __kvm_gmem_prepare_folio(struct kvm *kvm, struct kvm_memory_slot *slot,
				    pgoff_t index, struct folio *folio)
{
#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_PREPARE
	kvm_pfn_t pfn = folio_file_pfn(folio, index);
	gfn_t gfn = slot->base_gfn + index - slot->gmem.pgoff;
	int rc = kvm_arch_gmem_prepare(kvm, gfn, pfn, folio_order(folio));
	if (rc) {
		pr_warn_ratelimited("gmem: Failed to prepare folio for index %lx GFN %llx PFN %llx error %d.\n",
				    index, gfn, pfn, rc);
		return rc;
	}
#endif

	return 0;
}

/**
 * Use the uptodate flag to indicate that the folio is prepared for KVM's usage.
 */
static inline void kvm_gmem_mark_prepared(struct folio *folio)
{
	folio_mark_uptodate(folio);
}

/*
 * Process @folio, which contains @gfn, so that the guest can use it.
 * The folio must be locked and the gfn must be contained in @slot.
 * On successful return the guest sees a zero page so as to avoid
 * leaking host data and the up-to-date flag is set.
 */
static int kvm_gmem_prepare_folio(struct kvm *kvm, struct kvm_memory_slot *slot,
				  gfn_t gfn, struct folio *folio)
{
	pgoff_t index;
	int r;

	if (folio_test_hugetlb(folio)) {
		folio_zero_user(folio, folio->index << PAGE_SHIFT);
	} else {
		unsigned long nr_pages, i;

		nr_pages = folio_nr_pages(folio);
		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));
	}

	/*
	 * Preparing huge folios should always be safe, since it should
	 * be possible to split them later if needed.
	 *
	 * Right now the folio order is always going to be zero, but the
	 * code is ready for huge folios.  The only assumption is that
	 * the base pgoff of memslots is naturally aligned with the
	 * requested page order, ensuring that huge folios can also use
	 * huge page table entries for GPA->HPA mapping.
	 *
	 * The order will be passed when creating the guest_memfd, and
	 * checked when creating memslots.
	 */
	WARN_ON(!IS_ALIGNED(slot->gmem.pgoff, 1 << folio_order(folio)));
	index = gfn - slot->base_gfn + slot->gmem.pgoff;
	index = ALIGN_DOWN(index, 1 << folio_order(folio));
	r = __kvm_gmem_prepare_folio(kvm, slot, index, folio);
	if (!r)
		kvm_gmem_mark_prepared(folio);

	return r;
}

static int kvm_gmem_get_mpol_node_nodemask(gfp_t gfp_mask,
					   struct mempolicy **mpol,
					   nodemask_t **nodemask)
{
	/*
	 * TODO: mempolicy would probably have to be stored on the inode, use
	 * task policy for now.
	 */
	*mpol = get_task_policy(current);

	/* TODO: ignore interleaving (set ilx to 0) for now. */
	return policy_node_nodemask(*mpol, gfp_mask, 0, nodemask);
}

static struct folio *kvm_gmem_hugetlb_alloc_folio(struct hstate *h,
						  struct hugepage_subpool *spool)
{
	bool memcg_charge_was_prepared;
	struct mem_cgroup *memcg;
	struct mempolicy *mpol;
	nodemask_t *nodemask;
	struct folio *folio;
	gfp_t gfp_mask;
	int ret;
	int nid;

	gfp_mask = htlb_alloc_mask(h);

	memcg = get_mem_cgroup_from_current();
	ret = mem_cgroup_hugetlb_try_charge(memcg,
					    gfp_mask | __GFP_RETRY_MAYFAIL,
					    pages_per_huge_page(h));
	if (ret == -ENOMEM)
		goto err;

	memcg_charge_was_prepared = ret != -EOPNOTSUPP;

	/* Pages are only to be taken from guest_memfd subpool and nowhere else. */
	if (hugepage_subpool_get_pages(spool, 1))
		goto err_cancel_charge;

	nid = kvm_gmem_get_mpol_node_nodemask(htlb_alloc_mask(h), &mpol,
					      &nodemask);
	/*
	 * charge_cgroup_reservation is false because we didn't make any cgroup
	 * reservations when creating the guest_memfd subpool.
	 *
	 * use_hstate_resv is true because we reserved from global hstate when
	 * creating the guest_memfd subpool.
	 */
	folio = hugetlb_alloc_folio(h, mpol, nid, nodemask, false, true);
	mpol_cond_put(mpol);

	if (!folio)
		goto err_put_pages;

	hugetlb_set_folio_subpool(folio, spool);

	if (memcg_charge_was_prepared)
		mem_cgroup_commit_charge(folio, memcg);

out:
	mem_cgroup_put(memcg);

	return folio;

err_put_pages:
	hugepage_subpool_put_pages(spool, 1);

err_cancel_charge:
	if (memcg_charge_was_prepared)
		mem_cgroup_cancel_charge(memcg, pages_per_huge_page(h));

err:
	folio = ERR_PTR(-ENOMEM);
	goto out;
}

static int kvm_gmem_hugetlb_filemap_add_folio(struct address_space *mapping,
					      struct folio *folio, pgoff_t index,
					      gfp_t gfp)
{
	int ret;

	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, NULL);
	if (unlikely(ret)) {
		__folio_clear_locked(folio);
		return ret;
	}

	/*
	 * In hugetlb_add_to_page_cache(), there is a call to
	 * folio_clear_hugetlb_restore_reserve(). This is handled when the pages
	 * are removed from the page cache in unmap_hugepage_range() ->
	 * __unmap_hugepage_range() by conditionally calling
	 * folio_set_hugetlb_restore_reserve(). In kvm_gmem_hugetlb's usage of
	 * hugetlb, there are no VMAs involved, and pages are never taken from
	 * the surplus, so when pages are freed, the hstate reserve must be
	 * restored. Hence, this function makes no call to
	 * folio_clear_hugetlb_restore_reserve().
	 */

	/* mark folio dirty so that it will not be removed from cache/inode */
	folio_mark_dirty(folio);

	return 0;
}

struct kvm_gmem_split_stash {
	struct {
		unsigned long _flags_2;
		unsigned long _head_2;

		void *_hugetlb_subpool;
		void *_hugetlb_cgroup;
		void *_hugetlb_cgroup_rsvd;
		void *_hugetlb_hwpoison;
	};
	void *hugetlb_private;
};

static int kvm_gmem_hugetlb_stash_metadata(struct folio *folio)
{
	struct kvm_gmem_split_stash *stash;

	stash = kmalloc(sizeof(*stash), GFP_KERNEL);
	if (!stash)
		return -ENOMEM;

	stash->_flags_2 = folio->_flags_2;
	stash->_head_2 = folio->_head_2;
	stash->_hugetlb_subpool = folio->_hugetlb_subpool;
	stash->_hugetlb_cgroup = folio->_hugetlb_cgroup;
	stash->_hugetlb_cgroup_rsvd = folio->_hugetlb_cgroup_rsvd;
	stash->_hugetlb_hwpoison = folio->_hugetlb_hwpoison;
	stash->hugetlb_private = folio_get_private(folio);

	folio_change_private(folio, (void *)stash);

	return 0;
}

static int kvm_gmem_hugetlb_unstash_metadata(struct folio *folio)
{
	struct kvm_gmem_split_stash *stash;

	stash = folio_get_private(folio);

	if (!stash)
		return -EINVAL;

	folio->_flags_2 = stash->_flags_2;
	folio->_head_2 = stash->_head_2;
	folio->_hugetlb_subpool = stash->_hugetlb_subpool;
	folio->_hugetlb_cgroup = stash->_hugetlb_cgroup;
	folio->_hugetlb_cgroup_rsvd = stash->_hugetlb_cgroup_rsvd;
	folio->_hugetlb_hwpoison = stash->_hugetlb_hwpoison;
	folio_change_private(folio, stash->hugetlb_private);

	kfree(stash);

	return 0;
}

/**
 * Reconstruct a HugeTLB folio from a contiguous block of folios where the first
 * of the contiguous folios is @folio.
 *
 * The size of the contiguous block is of huge_page_size(@h). All the folios in
 * the block are checked to have a refcount of 1 before reconstruction. After
 * reconstruction, the reconstructed folio has a refcount of 1.
 *
 * Return 0 on success and negative error otherwise.
 */
static int kvm_gmem_hugetlb_reconstruct_folio(struct hstate *h, struct folio *folio)
{
	int ret;

	WARN_ON((folio->index & (huge_page_order(h) - 1)) != 0);

	ret = kvm_gmem_hugetlb_unstash_metadata(folio);
	if (ret)
		return ret;

	if (!prep_compound_gigantic_folio(folio, huge_page_order(h))) {
		kvm_gmem_hugetlb_stash_metadata(folio);
		return -ENOMEM;
	}

	__folio_set_hugetlb(folio);

	folio_set_count(folio, 1);

	hugetlb_vmemmap_optimize_folio(h, folio);

	return 0;
}

/* Basically folio_set_order(folio, 1) without the checks. */
static inline void kvm_gmem_folio_set_order(struct folio *folio, unsigned int order)
{
	folio->_flags_1 = (folio->_flags_1 & ~0xffUL) | order;
#ifdef CONFIG_64BIT
	folio->_folio_nr_pages = 1U << order;
#endif
}

/**
 * Split a HugeTLB @folio of size huge_page_size(@h).
 *
 * After splitting, each split folio has a refcount of 1. There are no checks on
 * refcounts before splitting.
 *
 * Return 0 on success and negative error otherwise.
 */
static int kvm_gmem_hugetlb_split_folio(struct hstate *h, struct folio *folio)
{
	int ret;

	ret = hugetlb_vmemmap_restore_folio(h, folio);
	if (ret)
		return ret;

	ret = kvm_gmem_hugetlb_stash_metadata(folio);
	if (ret) {
		hugetlb_vmemmap_optimize_folio(h, folio);
		return ret;
	}

	kvm_gmem_folio_set_order(folio, 0);

	destroy_compound_gigantic_folio(folio, huge_page_order(h));
	__folio_clear_hugetlb(folio);

	/*
	 * Remove the first folio from h->hugepage_activelist since it is no
	 * longer a HugeTLB page. The other split pages should not be on any
	 * lists.
	 */
	hugetlb_folio_list_del(folio);

	return 0;
}

static struct folio *kvm_gmem_hugetlb_alloc_and_cache_folio(struct inode *inode,
							    pgoff_t index)
{
	struct folio *allocated_hugetlb_folio;
	pgoff_t hugetlb_first_subpage_index;
	struct page *hugetlb_first_subpage;
	struct kvm_gmem_hugetlb *hgmem;
	struct page *requested_page;
	int ret;
	int i;

	hgmem = kvm_gmem_hgmem(inode);
	allocated_hugetlb_folio = kvm_gmem_hugetlb_alloc_folio(hgmem->h, hgmem->spool);
	if (IS_ERR(allocated_hugetlb_folio))
		return allocated_hugetlb_folio;

	requested_page = folio_file_page(allocated_hugetlb_folio, index);
	hugetlb_first_subpage = folio_file_page(allocated_hugetlb_folio, 0);
	hugetlb_first_subpage_index = index & (huge_page_mask(hgmem->h) >> PAGE_SHIFT);

	ret = kvm_gmem_hugetlb_split_folio(hgmem->h, allocated_hugetlb_folio);
	if (ret) {
		folio_put(allocated_hugetlb_folio);
		return ERR_PTR(ret);
	}

	for (i = 0; i < pages_per_huge_page(hgmem->h); ++i) {
		struct folio *folio = page_folio(nth_page(hugetlb_first_subpage, i));

		ret = kvm_gmem_hugetlb_filemap_add_folio(inode->i_mapping,
							 folio,
							 hugetlb_first_subpage_index + i,
							 htlb_alloc_mask(hgmem->h));
		if (ret) {
			/* TODO: handle cleanup properly. */
			pr_err("Handle cleanup properly index=%lx, ret=%d\n",
			       hugetlb_first_subpage_index + i, ret);
			dump_page(nth_page(hugetlb_first_subpage, i), "check");
			return ERR_PTR(ret);
		}

		/*
		 * Skip unlocking for the requested index since
		 * kvm_gmem_get_folio() returns a locked folio.
		 *
		 * Do folio_put() to drop the refcount that came with the folio,
		 * from splitting the folio. Splitting the folio has a refcount
		 * to be in line with hugetlb_alloc_folio(), which returns a
		 * folio with refcount 1.
		 *
		 * Skip folio_put() for requested index since
		 * kvm_gmem_get_folio() returns a folio with refcount 1.
		 */
		if (hugetlb_first_subpage_index + i != index) {
			folio_unlock(folio);
			folio_put(folio);
		}
	}

	spin_lock(&inode->i_lock);
	inode->i_blocks += blocks_per_huge_page(hgmem->h);
	spin_unlock(&inode->i_lock);

	return page_folio(requested_page);
}

static struct folio *kvm_gmem_get_hugetlb_folio(struct inode *inode,
						pgoff_t index)
{
	struct address_space *mapping;
	struct folio *folio;
	struct hstate *h;
	pgoff_t hindex;
	u32 hash;

	h = kvm_gmem_hgmem(inode)->h;
	hindex = index >> huge_page_order(h);
	mapping = inode->i_mapping;

	/* To lock, we calculate the hash using the hindex and not index. */
	hash = hugetlb_fault_mutex_hash(mapping, hindex);
	mutex_lock(&hugetlb_fault_mutex_table[hash]);

	/*
	 * The filemap is indexed with index and not hindex. Taking lock on
	 * folio to align with kvm_gmem_get_regular_folio()
	 */
	folio = filemap_lock_folio(mapping, index);
	if (!IS_ERR(folio))
		goto out;

	folio = kvm_gmem_hugetlb_alloc_and_cache_folio(inode, index);
out:
	mutex_unlock(&hugetlb_fault_mutex_table[hash]);

	return folio;
}

/*
 * Returns a locked folio on success.  The caller is responsible for
 * setting the up-to-date flag before the memory is mapped into the guest.
 * There is no backing storage for the memory, so the folio will remain
 * up-to-date until it's removed.
 *
 * Ignore accessed, referenced, and dirty flags.  The memory is
 * unevictable and there is no storage to write back to.
 */
static struct folio *kvm_gmem_get_folio(struct inode *inode, pgoff_t index)
{
	if (is_kvm_gmem_hugetlb(inode))
		return kvm_gmem_get_hugetlb_folio(inode, index);
	else
		return filemap_grab_folio(inode->i_mapping, index);
}

static void kvm_gmem_invalidate_begin(struct kvm_gmem *gmem, pgoff_t start,
				      pgoff_t end)
{
	bool flush = false, found_memslot = false;
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	xa_for_each_range(&gmem->bindings, index, slot, start, end - 1) {
		pgoff_t pgoff = slot->gmem.pgoff;

		struct kvm_gfn_range gfn_range = {
			.start = slot->base_gfn + max(pgoff, start) - pgoff,
			.end = slot->base_gfn + min(pgoff + slot->npages, end) - pgoff,
			.slot = slot,
			.may_block = true,
		};

		if (!found_memslot) {
			found_memslot = true;

			KVM_MMU_LOCK(kvm);
			kvm_mmu_invalidate_begin(kvm);
		}

		flush |= kvm_mmu_unmap_gfn_range(kvm, &gfn_range);
	}

	if (flush)
		kvm_flush_remote_tlbs(kvm);

	if (found_memslot)
		KVM_MMU_UNLOCK(kvm);
}

static void kvm_gmem_invalidate_end(struct kvm_gmem *gmem, pgoff_t start,
				    pgoff_t end)
{
	struct kvm *kvm = gmem->kvm;

	if (xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		KVM_MMU_LOCK(kvm);
		kvm_mmu_invalidate_end(kvm);
		KVM_MMU_UNLOCK(kvm);
	}
}

static inline void kvm_gmem_hugetlb_filemap_remove_folio(struct folio *folio)
{
	folio_lock(folio);

	folio_clear_dirty(folio);
	folio_clear_uptodate(folio);
	filemap_remove_folio(folio);

	folio_unlock(folio);
}

/**
 * Removes folios in range [@lstart, @lend) from page cache/filemap (@mapping),
 * returning the number of HugeTLB pages freed.
 *
 * @lend - @lstart must be a multiple of the HugeTLB page size.
 */
static int kvm_gmem_hugetlb_filemap_remove_folios(struct address_space *mapping,
						  struct hstate *h,
						  loff_t lstart, loff_t lend)
{
	const pgoff_t end = lend >> PAGE_SHIFT;
	pgoff_t next = lstart >> PAGE_SHIFT;
	LIST_HEAD(folios_to_reconstruct);
	struct folio_batch fbatch;
	struct folio *folio, *tmp;
	int num_freed = 0;
	int i;

	/*
	 * TODO: Iterate over huge_page_size(h) blocks to avoid taking and
	 * releasing hugetlb_fault_mutex_table[hash] lock so often. When
	 * truncating, lstart and lend should be clipped to the size of this
	 * guest_memfd file, otherwise there would be too many iterations.
	 */
	folio_batch_init(&fbatch);
	while (filemap_get_folios(mapping, &next, end - 1, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); ++i) {
			struct folio *folio;
			pgoff_t hindex;
			u32 hash;

			folio = fbatch.folios[i];

			hindex = folio->index >> huge_page_order(h);
			hash = hugetlb_fault_mutex_hash(mapping, hindex);
			mutex_lock(&hugetlb_fault_mutex_table[hash]);

			/*
			 * Collect first pages of HugeTLB folios for
			 * reconstruction later.
			 */
			if ((folio->index & ~(huge_page_mask(h) >> PAGE_SHIFT)) == 0)
				list_add(&folio->lru, &folios_to_reconstruct);

			/*
			 * Before removing from filemap, take a reference so
			 * sub-folios don't get freed. Don't free the sub-folios
			 * until after reconstruction.
			 */
			folio_get(folio);

			kvm_gmem_hugetlb_filemap_remove_folio(folio);

			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}

	list_for_each_entry_safe(folio, tmp, &folios_to_reconstruct, lru) {
		kvm_gmem_hugetlb_reconstruct_folio(h, folio);
		hugetlb_folio_list_move(folio, &h->hugepage_activelist);

		folio_put(folio);
		num_freed++;
	}

	return num_freed;
}

/**
 * Removes folios in range [@lstart, @lend) from page cache of inode, updates
 * inode metadata and hugetlb reservations.
 *
 * @lend - @lstart must be a multiple of the HugeTLB page size.
 */
static void kvm_gmem_hugetlb_truncate_folios_range(struct inode *inode,
						   loff_t lstart, loff_t lend)
{
	struct kvm_gmem_hugetlb *hgmem;
	struct hstate *h;
	int gbl_reserve;
	int num_freed;

	hgmem = kvm_gmem_hgmem(inode);
	h = hgmem->h;

	num_freed = kvm_gmem_hugetlb_filemap_remove_folios(inode->i_mapping,
							   h, lstart, lend);

	gbl_reserve = hugepage_subpool_put_pages(hgmem->spool, num_freed);
	hugetlb_acct_memory(h, -gbl_reserve);

	spin_lock(&inode->i_lock);
	inode->i_blocks -= blocks_per_huge_page(h) * num_freed;
	spin_unlock(&inode->i_lock);
}

/**
 * Zeroes offsets [@start, @end) in a folio from @mapping.
 *
 * [@start, @end) must be within the same folio.
 */
static void kvm_gmem_zero_partial_page(
	struct address_space *mapping, loff_t start, loff_t end)
{
	struct folio *folio;
	pgoff_t idx = start >> PAGE_SHIFT;

	folio = filemap_lock_folio(mapping, idx);
	if (IS_ERR(folio))
		return;

	start = offset_in_folio(folio, start);
	end = offset_in_folio(folio, end);
	if (!end)
		end = folio_size(folio);

	folio_zero_segment(folio, (size_t)start, (size_t)end);
	folio_unlock(folio);
	folio_put(folio);
}

/**
 * Zeroes all pages in range [@start, @end) in @mapping.
 *
 * hugetlb_zero_partial_page() would work if this had been a full page, but is
 * not suitable since the pages have been split.
 *
 * truncate_inode_pages_range() isn't the right function because it removes
 * pages from the page cache; this function only zeroes the pages.
 */
static void kvm_gmem_hugetlb_zero_split_pages(struct address_space *mapping,
					      loff_t start, loff_t end)
{
	loff_t aligned_start;
	loff_t index;

	aligned_start = round_up(start, PAGE_SIZE);

	kvm_gmem_zero_partial_page(mapping, start, min(aligned_start, end));

	for (index = aligned_start; index < end; index += PAGE_SIZE) {
		kvm_gmem_zero_partial_page(mapping, index,
					   min((loff_t)(index + PAGE_SIZE), end));
	}
}

static void kvm_gmem_hugetlb_truncate_range(struct inode *inode, loff_t lstart,
					    loff_t lend)
{
	loff_t full_hpage_start;
	loff_t full_hpage_end;
	unsigned long hsize;
	struct hstate *h;

	h = kvm_gmem_hgmem(inode)->h;
	hsize = huge_page_size(h);

	full_hpage_start = round_up(lstart, hsize);
	full_hpage_end = round_down(lend, hsize);

	if (lstart < full_hpage_start) {
		kvm_gmem_hugetlb_zero_split_pages(inode->i_mapping, lstart,
						  full_hpage_start);
	}

	if (full_hpage_end > full_hpage_start) {
		kvm_gmem_hugetlb_truncate_folios_range(inode, full_hpage_start,
						       full_hpage_end);
	}

	if (lend > full_hpage_end) {
		kvm_gmem_hugetlb_zero_split_pages(inode->i_mapping, full_hpage_end,
						  lend);
	}
}

static long kvm_gmem_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct list_head *gmem_list = &inode->i_mapping->i_private_list;
	pgoff_t start = offset >> PAGE_SHIFT;
	pgoff_t nr = len >> PAGE_SHIFT;
	pgoff_t end = start + nr;
	struct kvm_gmem *gmem;

	/*
	 * Bindings must be stable across invalidation to ensure the start+end
	 * are balanced.
	 */
	filemap_invalidate_lock(inode->i_mapping);

	/* TODO: Check if even_cows should be 0 or 1 */
	unmap_mapping_range(inode->i_mapping, start, len, 0);

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_begin(gmem, start, end);

	if (is_kvm_gmem_hugetlb(inode)) {
		kvm_gmem_hugetlb_truncate_range(inode, offset, offset + len);
	} else {
		truncate_inode_pages_range(inode->i_mapping, offset,
					   offset + len - 1);
	}

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_end(gmem, start, end);

	filemap_invalidate_unlock(inode->i_mapping);

	return 0;
}

static long kvm_gmem_allocate(struct inode *inode, loff_t offset, loff_t len)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	if (is_kvm_gmem_hugetlb(inode)) {
		unsigned long hsize = huge_page_size(kvm_gmem_hgmem(inode)->h);

		start = round_down(offset, hsize) >> PAGE_SHIFT;
		end = round_down(offset + len, hsize) >> PAGE_SHIFT;
	} else {
		start = offset >> PAGE_SHIFT;
		end = (offset + len) >> PAGE_SHIFT;
	}

	r = 0;
	for (index = start; index < end; ) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = kvm_gmem_get_folio(inode, index);
		if (IS_ERR(folio)) {
			r = PTR_ERR(folio);
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long kvm_gmem_fallocate(struct file *file, int mode, loff_t offset,
			       loff_t len)
{
	int ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = kvm_gmem_punch_hole(file_inode(file), offset, len);
	else
		ret = kvm_gmem_allocate(file_inode(file), offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int kvm_gmem_release(struct inode *inode, struct file *file)
{
	struct kvm_gmem *gmem = file->private_data;
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	/*
	 * Prevent concurrent attempts to *unbind* a memslot.  This is the last
	 * reference to the file and thus no new bindings can be created, but
	 * dereferencing the slot for existing bindings needs to be protected
	 * against memslot updates, specifically so that unbind doesn't race
	 * and free the memslot (kvm_gmem_get_file() will return NULL).
	 */
	mutex_lock(&kvm->slots_lock);

	filemap_invalidate_lock(inode->i_mapping);

	xa_for_each(&gmem->bindings, index, slot)
		rcu_assign_pointer(slot->gmem.file, NULL);

	synchronize_rcu();

	/*
	 * All in-flight operations are gone and new bindings can be created.
	 * Zap all SPTEs pointed at by this file.  Do not free the backing
	 * memory, as its lifetime is associated with the inode, not the file.
	 */
	kvm_gmem_invalidate_begin(gmem, 0, -1ul);
	kvm_gmem_invalidate_end(gmem, 0, -1ul);

	list_del(&gmem->entry);

	filemap_invalidate_unlock(inode->i_mapping);

	mutex_unlock(&kvm->slots_lock);

	xa_destroy(&gmem->bindings);
	kfree(gmem);

	kvm_put_kvm(kvm);

	return 0;
}

static inline struct file *kvm_gmem_get_file(struct kvm_memory_slot *slot)
{
	/*
	 * Do not return slot->gmem.file if it has already been closed;
	 * there might be some time between the last fput() and when
	 * kvm_gmem_release() clears slot->gmem.file, and you do not
	 * want to spin in the meanwhile.
	 */
	return get_file_active(&slot->gmem.file);
}

static void kvm_gmem_hugetlb_teardown(struct inode *inode)
{
	struct kvm_gmem_hugetlb *hgmem;

	/* TODO: Check if even_cows should be 0 or 1 */
	unmap_mapping_range(inode->i_mapping, 0, LLONG_MAX, 0);

	truncate_inode_pages_final_prepare(inode->i_mapping);
	kvm_gmem_hugetlb_truncate_folios_range(inode, 0, LLONG_MAX);

	hgmem = kvm_gmem_hgmem(inode);
	hugepage_put_subpool(hgmem->spool);
	kfree(hgmem);
}

static void kvm_gmem_evict_inode(struct inode *inode)
{
	struct kvm_gmem_inode_private *private = kvm_gmem_private(inode);

	/*
	 * .evict_inode can be called before faultability is set up if there are
	 * issues during inode creation.
	 */
	if (private)
		xa_destroy(&private->faultability);

	if (is_kvm_gmem_hugetlb(inode))
		kvm_gmem_hugetlb_teardown(inode);
	else
		truncate_inode_pages_final(inode->i_mapping);

	kfree(private);
	clear_inode(inode);
}

static const struct super_operations kvm_gmem_super_operations = {
	.statfs		= simple_statfs,
	.evict_inode	= kvm_gmem_evict_inode,
};

static int kvm_gmem_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	if (!init_pseudo(fc, GUEST_MEMORY_MAGIC))
		return -ENOMEM;

	ctx = fc->fs_private;
	ctx->ops = &kvm_gmem_super_operations;

	return 0;
}

static struct file_system_type kvm_gmem_fs = {
	.name		 = "kvm_guest_memory",
	.init_fs_context = kvm_gmem_init_fs_context,
	.kill_sb	 = kill_anon_super,
};

static void kvm_gmem_init_mount(void)
{
	kvm_gmem_mnt = kern_mount(&kvm_gmem_fs);
	BUG_ON(IS_ERR(kvm_gmem_mnt));

	kvm_gmem_mnt->mnt_flags |= MNT_NOEXEC;
}

static vm_fault_t kvm_gmem_fault(struct vm_fault *vmf)
{
	struct inode *inode;
	struct folio *folio;

	inode = file_inode(vmf->vma->vm_file);
	if (!kvm_gmem_is_faultable(inode, vmf->pgoff))
		return VM_FAULT_SIGBUS;

	folio = kvm_gmem_get_folio(inode, vmf->pgoff);
	if (!folio)
		return VM_FAULT_SIGBUS;

	vmf->page = folio_file_page(folio, vmf->pgoff);
	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct kvm_gmem_vm_ops = {
	.fault = kvm_gmem_fault,
};

static int kvm_gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) !=
	    (VM_SHARED | VM_MAYSHARE)) {
		return -EINVAL;
	}

	file_accessed(file);
	vm_flags_set(vma, VM_DONTDUMP);
	vma->vm_ops = &kvm_gmem_vm_ops;

	return 0;
}

static struct file_operations kvm_gmem_fops = {
	.mmap		= kvm_gmem_mmap,
	.open		= generic_file_open,
	.release	= kvm_gmem_release,
	.fallocate	= kvm_gmem_fallocate,
};

void kvm_gmem_init(struct module *module)
{
	kvm_gmem_fops.owner = module;

	kvm_gmem_init_mount();
}

static int kvm_gmem_migrate_folio(struct address_space *mapping,
				  struct folio *dst, struct folio *src,
				  enum migrate_mode mode)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

static int kvm_gmem_error_folio(struct address_space *mapping, struct folio *folio)
{
	struct list_head *gmem_list = &mapping->i_private_list;
	struct kvm_gmem *gmem;
	pgoff_t start, end;

	filemap_invalidate_lock_shared(mapping);

	start = folio->index;
	end = start + folio_nr_pages(folio);

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_begin(gmem, start, end);

	/*
	 * Do not truncate the range, what action is taken in response to the
	 * error is userspace's decision (assuming the architecture supports
	 * gracefully handling memory errors).  If/when the guest attempts to
	 * access a poisoned page, kvm_gmem_get_pfn() will return -EHWPOISON,
	 * at which point KVM can either terminate the VM or propagate the
	 * error to userspace.
	 */

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_end(gmem, start, end);

	filemap_invalidate_unlock_shared(mapping);

	return MF_DELAYED;
}

#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_INVALIDATE
static void kvm_gmem_free_folio(struct folio *folio)
{
	struct page *page = folio_page(folio, 0);
	kvm_pfn_t pfn = page_to_pfn(page);
	int order = folio_order(folio);

	kvm_arch_gmem_invalidate(pfn, pfn + (1ul << order));
}
#endif

static const struct address_space_operations kvm_gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio	= kvm_gmem_migrate_folio,
	.error_remove_folio = kvm_gmem_error_folio,
#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_INVALIDATE
	.free_folio = kvm_gmem_free_folio,
#endif
};

static int kvm_gmem_getattr(struct mnt_idmap *idmap, const struct path *path,
			    struct kstat *stat, u32 request_mask,
			    unsigned int query_flags)
{
	struct inode *inode = path->dentry->d_inode;

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static int kvm_gmem_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	return -EINVAL;
}
static const struct inode_operations kvm_gmem_iops = {
	.getattr	= kvm_gmem_getattr,
	.setattr	= kvm_gmem_setattr,
};

static int kvm_gmem_hugetlb_setup(struct inode *inode,
				  struct kvm_gmem_inode_private *private,
				  loff_t size, u64 flags)
{
	struct kvm_gmem_hugetlb *hgmem;
	struct hugepage_subpool *spool;
	int page_size_log;
	struct hstate *h;
	long hpages;

	hgmem = kzalloc(sizeof(*hgmem), GFP_KERNEL);
	if (!hgmem)
		return -ENOMEM;

	page_size_log = (flags >> KVM_GUEST_MEMFD_HUGE_SHIFT) & KVM_GUEST_MEMFD_HUGE_MASK;
	h = hstate_sizelog(page_size_log);

	/* Round up to accommodate size requests that don't align with huge pages */
	hpages = round_up(size, huge_page_size(h)) >> huge_page_shift(h);

	spool = hugepage_new_subpool(h, hpages, hpages, false);
	if (!spool)
		goto err;

	inode->i_blkbits = huge_page_shift(h);

	hgmem->h = h;
	hgmem->spool = spool;

	private->hgmem = hgmem;
	return 0;

err:
	kfree(hgmem);
	return -ENOMEM;
}

static struct inode *kvm_gmem_inode_make_secure_inode(const char *name,
						      loff_t size, u64 flags)
{
	const struct qstr qname = QSTR_INIT(name, strlen(name));
	struct kvm_gmem_inode_private *private;
	struct inode *inode;
	int err;

	inode = alloc_anon_inode(kvm_gmem_mnt->mnt_sb);
	if (IS_ERR(inode))
		return inode;

	err = security_inode_init_security_anon(inode, &qname, NULL);
	if (err)
		goto out;

	err = -ENOMEM;
	private = kzalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		goto out;

	if (flags & KVM_GUEST_MEMFD_HUGETLB) {
		err = kvm_gmem_hugetlb_setup(inode, private, size, flags);
		if (err)
			goto free_private;
	}

	xa_init(&private->faultability);
	inode->i_mapping->i_private_data = private;

	inode->i_private = (void *)(unsigned long)flags;
	inode->i_op = &kvm_gmem_iops;
	inode->i_mapping->a_ops = &kvm_gmem_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_inaccessible(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	return inode;

free_private:
	kfree(private);
out:
	iput(inode);

	return ERR_PTR(err);
}

static struct file *kvm_gmem_inode_create_getfile(void *priv, loff_t size,
						  u64 flags)
{
	static const char *name = "[kvm-gmem]";
	struct inode *inode;
	struct file *file;

	if (kvm_gmem_fops.owner && !try_module_get(kvm_gmem_fops.owner))
		return ERR_PTR(-ENOENT);

	inode = kvm_gmem_inode_make_secure_inode(name, size, flags);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	file = alloc_file_pseudo(inode, kvm_gmem_mnt, name, O_RDWR,
				 &kvm_gmem_fops);
	if (IS_ERR(file)) {
		iput(inode);
		return file;
	}

	file->f_mapping = inode->i_mapping;
	file->f_flags |= O_LARGEFILE;
	file->private_data = priv;

	return file;
}

static void kvm_gmem_set_default_faultability_by_vm_type(struct inode *inode,
							 u8 vm_type,
							 loff_t start, loff_t end)
{
	bool faultable;

	switch (vm_type) {
	case KVM_X86_SW_PROTECTED_VM:
		faultable = true;
		break;
	default:
		faultable = false;
	}

	WARN_ON(kvm_gmem_set_faultable(inode, start, end, faultable));
}

static int __kvm_gmem_create(struct kvm *kvm, loff_t size, u64 flags)
{
	struct kvm_gmem *gmem;
	struct file *file;
	int fd, err;

	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;

	gmem = kzalloc(sizeof(*gmem), GFP_KERNEL);
	if (!gmem) {
		err = -ENOMEM;
		goto err_fd;
	}

	file = kvm_gmem_inode_create_getfile(gmem, size, flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_gmem;
	}

	kvm_get_kvm(kvm);
	gmem->kvm = kvm;
	xa_init(&gmem->bindings);
	list_add(&gmem->entry, &file_inode(file)->i_mapping->i_private_list);

	fd_install(fd, file);
	return fd;

err_gmem:
	kfree(gmem);
err_fd:
	put_unused_fd(fd);
	return err;
}

static inline bool kvm_gmem_hugetlb_page_aligned(u32 flags, u64 value)
{
	int page_size_log = (flags >> KVM_GUEST_MEMFD_HUGE_SHIFT) & KVM_GUEST_MEMFD_HUGE_MASK;
	u64 page_size = 1ULL << page_size_log;
	return IS_ALIGNED(value, page_size);
}

#define KVM_GUEST_MEMFD_ALL_FLAGS KVM_GUEST_MEMFD_HUGETLB

int kvm_gmem_create(struct kvm *kvm, struct kvm_create_guest_memfd *args)
{
	loff_t size = args->size;
	u64 flags = args->flags;

	if (flags & KVM_GUEST_MEMFD_HUGETLB) {
		/* Allow huge page size encoding in flags */
		if (flags & ~(KVM_GUEST_MEMFD_ALL_FLAGS |
			      (KVM_GUEST_MEMFD_HUGE_MASK << KVM_GUEST_MEMFD_HUGE_SHIFT)))
			return -EINVAL;

		if (!kvm_gmem_hugetlb_page_aligned(flags, size))
			return -EINVAL;
	} else {
		if (flags & ~KVM_GUEST_MEMFD_ALL_FLAGS)
			return -EINVAL;

		if (!PAGE_ALIGNED(size))
			return -EINVAL;
	}

	if (size <= 0)
		return -EINVAL;

	return __kvm_gmem_create(kvm, size, flags);
}

int kvm_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		  unsigned int fd, loff_t offset)
{
	loff_t size = slot->npages << PAGE_SHIFT;
	unsigned long start, end;
	struct kvm_gmem *gmem;
	struct inode *inode;
	struct file *file;
	int r = -EINVAL;

	BUILD_BUG_ON(sizeof(gfn_t) != sizeof(slot->gmem.pgoff));

	file = fget(fd);
	if (!file)
		return -EBADF;

	if (file->f_op != &kvm_gmem_fops)
		goto err;

	gmem = file->private_data;
	if (gmem->kvm != kvm)
		goto err;

	inode = file_inode(file);

	if (offset < 0 || !PAGE_ALIGNED(offset) ||
	    offset + size > i_size_read(inode))
		goto err;

	filemap_invalidate_lock(inode->i_mapping);

	start = offset >> PAGE_SHIFT;
	end = start + slot->npages;

	if (!xa_empty(&gmem->bindings) &&
	    xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		filemap_invalidate_unlock(inode->i_mapping);
		goto err;
	}

	/*
	 * No synchronize_rcu() needed, any in-flight readers are guaranteed to
	 * be see either a NULL file or this new file, no need for them to go
	 * away.
	 */
	rcu_assign_pointer(slot->gmem.file, file);
	slot->gmem.pgoff = start;

	xa_store_range(&gmem->bindings, start, end - 1, slot, GFP_KERNEL);

	kvm_gmem_set_default_faultability_by_vm_type(file_inode(file),
						     kvm->arch.vm_type,
						     start, end);

	filemap_invalidate_unlock(inode->i_mapping);

	/*
	 * Drop the reference to the file, even on success.  The file pins KVM,
	 * not the other way 'round.  Active bindings are invalidated if the
	 * file is closed before memslots are destroyed.
	 */
	r = 0;
err:
	fput(file);
	return r;
}

void kvm_gmem_unbind(struct kvm_memory_slot *slot)
{
	unsigned long start = slot->gmem.pgoff;
	unsigned long end = start + slot->npages;
	struct kvm_gmem *gmem;
	struct file *file;

	/*
	 * Nothing to do if the underlying file was already closed (or is being
	 * closed right now), kvm_gmem_release() invalidates all bindings.
	 */
	file = kvm_gmem_get_file(slot);
	if (!file)
		return;

	gmem = file->private_data;

	filemap_invalidate_lock(file->f_mapping);
	xa_store_range(&gmem->bindings, start, end - 1, NULL, GFP_KERNEL);
	rcu_assign_pointer(slot->gmem.file, NULL);
	synchronize_rcu();
	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
}

/* Returns a locked folio on success.  */
static struct folio *
__kvm_gmem_get_pfn(struct file *file, struct kvm_memory_slot *slot,
		   gfn_t gfn, kvm_pfn_t *pfn, bool *is_prepared,
		   int *max_order)
{
	pgoff_t index = gfn - slot->base_gfn + slot->gmem.pgoff;
	struct kvm_gmem *gmem = file->private_data;
	struct folio *folio;

	if (file != slot->gmem.file) {
		WARN_ON_ONCE(slot->gmem.file);
		return ERR_PTR(-EFAULT);
	}

	gmem = file->private_data;
	if (xa_load(&gmem->bindings, index) != slot) {
		WARN_ON_ONCE(xa_load(&gmem->bindings, index));
		return ERR_PTR(-EIO);
	}

	folio = kvm_gmem_get_folio(file_inode(file), index);
	if (IS_ERR(folio))
		return folio;

	if (folio_test_hwpoison(folio)) {
		folio_unlock(folio);
		/*
		 * TODO: this folio may be part of a HugeTLB folio. Perhaps
		 * reconstruct and then free page?
		 */
		folio_put(folio);
		return ERR_PTR(-EHWPOISON);
	}

	*pfn = folio_file_pfn(folio, index);
	if (max_order)
		*max_order = folio_order(folio);

	*is_prepared = folio_test_uptodate(folio);
	return folio;
}

int kvm_gmem_get_pfn(struct kvm *kvm, struct kvm_memory_slot *slot,
		     gfn_t gfn, kvm_pfn_t *pfn, int *max_order)
{
	struct file *file = kvm_gmem_get_file(slot);
	struct folio *folio;
	bool is_prepared = false;
	int r = 0;

	if (!file)
		return -EFAULT;

	folio = __kvm_gmem_get_pfn(file, slot, gfn, pfn, &is_prepared, max_order);
	if (IS_ERR(folio)) {
		r = PTR_ERR(folio);
		goto out;
	}

	if (!is_prepared)
		r = kvm_gmem_prepare_folio(kvm, slot, gfn, folio);

	folio_unlock(folio);
	if (r < 0)
		folio_put(folio);

out:
	fput(file);
	return r;
}
EXPORT_SYMBOL_GPL(kvm_gmem_get_pfn);

#ifdef CONFIG_KVM_GENERIC_PRIVATE_MEM
long kvm_gmem_populate(struct kvm *kvm, gfn_t start_gfn, void __user *src, long npages,
		       kvm_gmem_populate_cb post_populate, void *opaque)
{
	struct file *file;
	struct kvm_memory_slot *slot;
	void __user *p;

	int ret = 0, max_order;
	long i;

	lockdep_assert_held(&kvm->slots_lock);
	if (npages < 0)
		return -EINVAL;

	slot = gfn_to_memslot(kvm, start_gfn);
	if (!kvm_slot_can_be_private(slot))
		return -EINVAL;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return -EFAULT;

	filemap_invalidate_lock(file->f_mapping);

	npages = min_t(ulong, slot->npages - (start_gfn - slot->base_gfn), npages);
	for (i = 0; i < npages; i += (1 << max_order)) {
		struct folio *folio;
		gfn_t gfn = start_gfn + i;
		bool is_prepared = false;
		kvm_pfn_t pfn;

		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		folio = __kvm_gmem_get_pfn(file, slot, gfn, &pfn, &is_prepared, &max_order);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			break;
		}

		if (is_prepared) {
			folio_unlock(folio);
			folio_put(folio);
			ret = -EEXIST;
			break;
		}

		folio_unlock(folio);
		WARN_ON(!IS_ALIGNED(gfn, 1 << max_order) ||
			(npages - i) < (1 << max_order));

		ret = -EINVAL;
		while (!kvm_range_has_memory_attributes(kvm, gfn, gfn + (1 << max_order),
							KVM_MEMORY_ATTRIBUTE_PRIVATE,
							KVM_MEMORY_ATTRIBUTE_PRIVATE)) {
			if (!max_order)
				goto put_folio_and_exit;
			max_order--;
		}

		p = src ? src + i * PAGE_SIZE : NULL;
		ret = post_populate(kvm, gfn, pfn, p, max_order, opaque);
		if (!ret)
			kvm_gmem_mark_prepared(folio);

put_folio_and_exit:
		folio_put(folio);
		if (ret)
			break;
	}

	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
	return ret && !i ? ret : i;
}
EXPORT_SYMBOL_GPL(kvm_gmem_populate);

/**
 * Returns true if pages in range [@start, @end) in inode @inode have no
 * userspace mappings.
 */
static bool kvm_gmem_no_mappings_range(struct inode *inode, pgoff_t start, pgoff_t end)
{
	pgoff_t index;
	bool checked_indices_unmapped;

	filemap_invalidate_lock_shared(inode->i_mapping);

	/* TODO: replace iteration with filemap_get_folios() for efficiency. */
	checked_indices_unmapped = true;
	for (index = start; checked_indices_unmapped && index < end;) {
		struct folio *folio;

		/* Don't use kvm_gmem_get_folio to avoid allocating */
		folio = filemap_lock_folio(inode->i_mapping, index);
		if (IS_ERR(folio)) {
			++index;
			continue;
		}

		if (folio_mapped(folio) || folio_maybe_dma_pinned(folio))
			checked_indices_unmapped = false;
		else
			index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);
	}

	filemap_invalidate_unlock_shared(inode->i_mapping);
	return checked_indices_unmapped;
}

/**
 * Returns true if pages in range [@start, @end) in memslot @slot have no
 * userspace mappings.
 */
static bool kvm_gmem_no_mappings_slot(struct kvm_memory_slot *slot,
				      gfn_t start, gfn_t end)
{
	pgoff_t offset_start;
	pgoff_t offset_end;
	struct file *file;
	bool ret;

	offset_start = start - slot->base_gfn + slot->gmem.pgoff;
	offset_end = end - slot->base_gfn + slot->gmem.pgoff;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return false;

	ret = kvm_gmem_no_mappings_range(file_inode(file), offset_start, offset_end);

	fput(file);

	return ret;
}

/**
 * Returns true if pages in range [@start, @end) have no host userspace mappings.
 */
static bool kvm_gmem_no_mappings(struct kvm *kvm, gfn_t start, gfn_t end)
{
	int i;

	lockdep_assert_held(&kvm->slots_lock);

	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		struct kvm_memslot_iter iter;
		struct kvm_memslots *slots;

		slots = __kvm_memslots(kvm, i);
		kvm_for_each_memslot_in_gfn_range(&iter, slots, start, end) {
			struct kvm_memory_slot *slot;
			gfn_t gfn_start;
			gfn_t gfn_end;

			slot = iter.slot;
			gfn_start = max(start, slot->base_gfn);
			gfn_end = min(end, slot->base_gfn + slot->npages);

			if (iter.slot->flags & KVM_MEM_GUEST_MEMFD &&
			    !kvm_gmem_no_mappings_slot(iter.slot, gfn_start, gfn_end))
				return false;
		}
	}

	return true;
}

/**
 * Set faultability of given range of gfns [@start, @end) in memslot @slot to
 * @faultable.
 */
static void kvm_gmem_set_faultable_slot(struct kvm_memory_slot *slot, gfn_t start,
					gfn_t end, bool faultable)
{
	pgoff_t start_offset;
	pgoff_t end_offset;
	struct file *file;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return;

	start_offset = start - slot->base_gfn + slot->gmem.pgoff;
	end_offset = end - slot->base_gfn + slot->gmem.pgoff;

	WARN_ON(kvm_gmem_set_faultable(file_inode(file), start_offset, end_offset,
				       faultable));

	fput(file);
}

/**
 * Set faultability of given range of gfns [@start, @end) in memslot @slot to
 * @faultable.
 */
static void kvm_gmem_set_faultable_vm(struct kvm *kvm, gfn_t start, gfn_t end,
				      bool faultable)
{
	int i;

	lockdep_assert_held(&kvm->slots_lock);

	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		struct kvm_memslot_iter iter;
		struct kvm_memslots *slots;

		slots = __kvm_memslots(kvm, i);
		kvm_for_each_memslot_in_gfn_range(&iter, slots, start, end) {
			struct kvm_memory_slot *slot;
			gfn_t gfn_start;
			gfn_t gfn_end;

			slot = iter.slot;
			gfn_start = max(start, slot->base_gfn);
			gfn_end = min(end, slot->base_gfn + slot->npages);

			if (iter.slot->flags & KVM_MEM_GUEST_MEMFD) {
				kvm_gmem_set_faultable_slot(slot, gfn_start,
							    gfn_end, faultable);
			}
		}
	}
}

/**
 * Returns true if guest_memfd permits setting range [@start, @end) to PRIVATE.
 *
 * If memory is faulted in to host userspace and a request was made to set the
 * memory to PRIVATE, the faulted in pages must not be pinned for the request to
 * be permitted.
 */
static int kvm_gmem_should_set_attributes_private(struct kvm *kvm, gfn_t start,
						  gfn_t end)
{
	kvm_gmem_set_faultable_vm(kvm, start, end, false);

	if (kvm_gmem_no_mappings(kvm, start, end))
		return 0;

	kvm_gmem_set_faultable_vm(kvm, start, end, true);
	return -EINVAL;
}

/**
 * Returns true if guest_memfd permits setting range [@start, @end) to SHARED.
 *
 * Because this allows pages to be faulted in to userspace, this must only be
 * called after the pages have been invalidated from guest page tables.
 */
static int kvm_gmem_should_set_attributes_shared(struct kvm *kvm, gfn_t start,
						 gfn_t end)
{
	/* Always okay to set shared, hence set range faultable here. */
	kvm_gmem_set_faultable_vm(kvm, start, end, true);

	return 0;
}

/**
 * Returns 0 if guest_memfd permits setting attributes @attrs for range [@start,
 * @end) or negative error otherwise.
 *
 * If memory is faulted in to host userspace and a request was made to set the
 * memory to PRIVATE, the faulted in pages must not be pinned for the request to
 * be permitted.
 *
 * Because this may allow pages to be faulted in to userspace when requested to
 * set attributes to shared, this must only be called after the pages have been
 * invalidated from guest page tables.
 */
int kvm_gmem_should_set_attributes(struct kvm *kvm, gfn_t start, gfn_t end,
				   unsigned long attrs)
{
	if (attrs & KVM_MEMORY_ATTRIBUTE_PRIVATE)
		return kvm_gmem_should_set_attributes_private(kvm, start, end);
	else
		return kvm_gmem_should_set_attributes_shared(kvm, start, end);
}

#endif
