// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * DMA_PMD per-domain IOVA window management and IOMMU mapping.
 *
 * See Documentation/core-api/dma-pmd.rst for the architecture overview.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dma-pmd.h>
#include <linux/export.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>

#include "dma-iommu.h"
#include "dma-pmd-priv.h"

/*
 * Index -> domain, for PMD pages that have to unmap themselves from every domain
 * that installed them. A slot is cleared before the matching bits are cleared
 * in dma_pmd_domain_release(), so a PMD page racing with a dying domain sees
 * NULL and correctly leaves the doomed page tables alone.
 *
 * @base is cached here rather than read back from the domain's DMA cookie, so
 * that teardown never has to dereference a cookie that may be about to be
 * freed.
 *
 * The lock also serialises index allocation, which happens from the DMA map
 * path and so cannot sleep.
 */
static struct {
	struct iommu_domain *domain;
	dma_addr_t base;
	bool draining;
} dma_pmd_domains[DMA_PMD_MAX_DOMAINS] __read_mostly;
static DEFINE_SPINLOCK(dma_pmd_domains_lock);

/*
 * Held by dma_pmd_unmap_all() across the registry read and the iommu_unmap()
 * that follows, and waited on by dma_pmd_domain_release() once it has cleared
 * the slot. Without it an unmapper that sampled a domain a moment before the
 * slot was cleared would walk into page tables their owner is already freeing.
 * SRCU rather than RCU to avoid holding RCU across IOTLB flushes.
 */
DEFINE_SRCU(dma_pmd_srcu);

bool dma_pmd_domain_active(unsigned int idx, const struct iommu_domain *domain)
{
	return READ_ONCE(dma_pmd_domains[idx].domain) == domain;
}

/**
 * dma_pmd_unmap_all - Remove a PMD page's PTE from every domain holding it
 * @meta: PMD page metadata structure
 *
 * The window itself is a permanent allocation out of each domain's
 * iova_domain and is deliberately left alone; only the leaf PTE goes away.
 *
 * The bits are cleared before the unmaps, not after. A mapper that observes a
 * cleared bit takes the slow path and blocks on @meta->map_lock, so it either
 * re-installs the PTE or waits; the reverse order would leave a window in
 * which the bit still claims a translation that has already been torn down.
 *
 * A domain that has since been released has a NULL registry slot and is
 * skipped: its page tables are being freed by their owner and must not be
 * touched here.
 *
 * Cost: Slow path (teardown / shrinker only); one iommu_unmap() per domain.
 * Locking: Acquires @meta->map_lock and @dma_pmd_domains_lock (irqsave), and
 *          drops both before unmapping. Holds @dma_pmd_srcu across the
 *          registry read and the unmap, which is what keeps the domain alive
 *          in between.
 * Frequency: Rare (only when a PMD page is released back to the buddy
 *            allocator or when a pool is destroyed).
 */
void dma_pmd_unmap_all(struct dma_pmd_meta *meta)
{
	dma_addr_t offset = dma_pmd_meta_win_offset(meta);
	unsigned long mapped, flags;
	int idx, srcu_idx;

	srcu_idx = srcu_read_lock(&dma_pmd_srcu);
	spin_lock_irqsave(&meta->map_lock, flags);
	mapped = meta->domains_mapped;
	meta->domains_mapped = 0;
	spin_unlock_irqrestore(&meta->map_lock, flags);

	if (!mapped) {
		srcu_read_unlock(&dma_pmd_srcu, srcu_idx);
		return;
	}

	for_each_set_bit(idx, &mapped, DMA_PMD_MAX_DOMAINS) {
		struct iommu_domain *domain;
		dma_addr_t base;

		spin_lock_irqsave(&dma_pmd_domains_lock, flags);
		domain = dma_pmd_domains[idx].domain;
		base = dma_pmd_domains[idx].base;
		spin_unlock_irqrestore(&dma_pmd_domains_lock, flags);

		if (!domain)
			continue;

		iommu_unmap(domain, base + offset, PMD_SIZE);
	}
	srcu_read_unlock(&dma_pmd_srcu, srcu_idx);
}

/**
 * dma_pmd_forget_domain - Drop a PMD page's PTE record for a dying domain
 * @meta: PMD page metadata structure
 * @idx: Index of the domain being torn down
 *
 * Deliberately does not unmap: @domain is being destroyed by its owner, which
 * frees the page tables and the IOVA domain wholesale. Touching it here is
 * exactly the use-after-free this is meant to prevent, so the bit is only
 * forgotten.
 *
 * Locking: Acquires @meta->map_lock (irqsave).
 *
 * Return: 1 if this PMD page had a mapping in @idx, 0 otherwise.
 */
unsigned int dma_pmd_forget_domain(struct dma_pmd_meta *meta, int idx)
{
	unsigned int dropped = 0;
	unsigned long flags;

	if (!(READ_ONCE(meta->domains_mapped) & BIT(idx)))
		return 0;

	spin_lock_irqsave(&meta->map_lock, flags);
	if (meta->domains_mapped & BIT(idx)) {
		meta->domains_mapped &= ~BIT(idx);
		dropped = 1;
	}
	spin_unlock_irqrestore(&meta->map_lock, flags);

	return dropped;
}

/**
 * dma_pmd_domain_release - Forget every PMD page mapping for @domain
 * @domain: DMA domain being destroyed
 *
 * A PMD page records, as a bit per domain index, which domains hold its PMD leaf
 * PTE. Those bits are otherwise only cleared when the PMD page itself is
 * released, so a domain that comes and goes - which this tree does at runtime,
 * see the DMS comments in __iommu_dma_map_phys() - would permanently consume
 * one index per generation, and every PMD page that ever mapped through it would
 * keep claiming a mapping in page tables that no longer exist.
 *
 * The registry slot is cleared first and the per-PMD-page bits afterwards.
 * dma_pmd_unmap_all() reads the bit and then the slot, so that order means it
 * either sees no bit, or sees a NULL slot and skips: it can never unmap
 * through a domain whose page tables its owner is freeing. An unmapper that
 * sampled the slot just before it was cleared is waited out on @dma_pmd_srcu.
 *
 * The slot stays reserved until the walk and drain are done, so the index
 * cannot be handed to a new domain while PMD pages still carry its bit.
 *
 * Cost: O(PMD pages) across all pools, but only on domain teardown.
 * Locking: Takes @dma_pmd_domains_lock, then @dma_pmd_pools_lock, then each
 *          @pool->lock, then each @meta->map_lock. Must be called from process
 *          context.
 */
void dma_pmd_domain_release(struct iommu_domain *domain)
{
	unsigned int dropped = 0;
	unsigned long flags;
	unsigned int i;
	int idx;

	/* Nothing has ever been pooled, so nothing can reference @domain. */
	if (likely(!dma_pmd_meta_base()))
		return;

	spin_lock_irqsave(&dma_pmd_domains_lock, flags);
	for (idx = 0; idx < DMA_PMD_MAX_DOMAINS; idx++) {
		if (dma_pmd_domains[idx].domain == domain) {
			dma_pmd_domains[idx].domain = NULL;
			/*
			 * Keep the slot reserved until every PMD page below has
			 * forgotten it. A new domain handed this index now
			 * would inherit the stale bits, and its first map of
			 * such a PMD page would skip iommu_map() and return an
			 * IOVA with no PTE behind it.
			 */
			dma_pmd_domains[idx].draining = true;
			break;
		}
	}
	spin_unlock_irqrestore(&dma_pmd_domains_lock, flags);

	/* @domain never pooled, so no PMD page can be holding a bit for it. */
	if (idx == DMA_PMD_MAX_DOMAINS)
		return;

	dropped += dma_pmd_pools_forget_domain(idx);

	for (i = 0; i < DMA_PMD_ARENA_PAGES; i++)
		dropped += dma_pmd_forget_domain(dma_pmd_arena_meta(i), idx);

	/*
	 * Any PMD page removed from a pool list before the walk above is either
	 * already on @dma_pmd_free_list (placed there under @pool->lock in
	 * __dma_pmd_free_page()) or being unmapped under @dma_pmd_srcu (in
	 * the shrinker or dma_pmd_pool_destroy()). Drain the reclaim list and
	 * wait out @dma_pmd_srcu before allowing @idx to be reused.
	 *
	 * synchronize_srcu() also waits out any dma_pmd_unmap_all() that
	 * sampled @domain before the slot was cleared above.
	 */
	synchronize_srcu(&dma_pmd_srcu);

	/* No PMD page claims @idx any more, so it can be reused. */
	spin_lock_irqsave(&dma_pmd_domains_lock, flags);
	dma_pmd_domains[idx].draining = false;
	spin_unlock_irqrestore(&dma_pmd_domains_lock, flags);

	if (dropped)
		pr_debug("dma_pmd: released %u PMD page mappings for domain %p (index %d)\n",
			 dropped, domain, idx);
}

/**
 * dma_pmd_dma_window_alloc - Allocate an IOVA window for DMA_PMD mappings
 * @dev: Device whose addressing limits the window must respect
 * @domain: Domain to take the window from
 * @sizep: In/out window size in bytes
 *
 * Allocates a contiguous IOVA range out of @domain's iova_domain on first use,
 * aligned to PMD_SIZE matching the mapping. Callers on the map path still
 * check each device's own dma_mask/bus_dma_limit against the resulting IOVA and
 * fall back to per-buffer mapping if a narrower device shares @domain.
 *
 * Return: base IOVA, or 0 if no range that large is available.
 */
static dma_addr_t dma_pmd_dma_window_alloc(struct device *dev,
					   struct iommu_domain *domain, u64 *sizep)
{
	u64 min_size = ARENA_REGION_SIZE + ALIGN(PFN_PHYS(max_pfn), PMD_SIZE);
	u64 limit = min_not_zero(dma_get_mask(dev), dev->bus_dma_limit);
	struct iova_domain *iovad = dma_pmd_dma_iovad(domain);
	unsigned long shift, iova_len;
	struct iova *new_iova;

	if (!iovad)
		return 0;

	if (domain->geometry.force_aperture)
		limit = min_t(u64, limit, domain->geometry.aperture_end);

	if (*sizep > limit / 2 && limit / 2 >= min_size)
		*sizep = ALIGN_DOWN(limit / 2, PMD_SIZE);

	/* Ignore devices with too many constraints. */
	if (limit <= DMA_BIT_MASK(32) || *sizep > limit - PMD_SIZE)
		return 0;

	shift = iova_shift(iovad);
	iova_len = (*sizep + PMD_SIZE) >> shift;

	/*
	 * Note: dev->iommu->pci_32bit_workaround is intentionally not consulted
	 * here. It is an advisory preference (enabled by default on all PCI
	 * devices).
	 */
	new_iova = alloc_iova(iovad, iova_len, limit >> shift, false);
	if (!new_iova)
		return 0;

	return ALIGN((dma_addr_t)new_iova->pfn_lo << shift, PMD_SIZE);
}

/**
 * dma_pmd_window_assign - Give @domain an IOVA window and a domain index
 * @dev: Device whose addressing limits the window must respect
 * @domain: Domain to set up
 * @win: @domain's window, from its DMA cookie
 *
 * Called the first time anything tries to pool through @domain. Failure is
 * recorded in @win->domain_idx rather than retried: the allocation is a
 * multi-terabyte search of the IOVA tree, and a domain that has no room for it
 * once will not have room on the next buffer either. The caller tests that
 * record without the lock, so the reason has to survive in the sentinel.
 *
 * A domain that cannot install a PMD leaf is declined. The whole benefit
 * is the single large PTE; without it a 2M page would cost 512 4KB PTEs, which
 * is worse than not pooling.
 *
 * Locking: Acquires @dma_pmd_domains_lock (irqsave). Callable from the DMA
 *          map path, so it never sleeps.
 *
 * Return: 0 if @win is usable on return, negative otherwise.
 */
int dma_pmd_window_assign(struct device *dev, struct iommu_domain *domain,
			  struct dma_pmd_window *win)
{
	u64 size = ALIGN(PFN_PHYS(dma_pmd_top_pfn()), PMD_SIZE) + ARENA_REGION_SIZE;
	unsigned long flags;
	int idx, ret = 0;
	dma_addr_t base;

	if (!dev_is_dma_coherent(dev))
		return -EOPNOTSUPP;

	if (min_not_zero(dma_get_mask(dev), dev->bus_dma_limit) <= DMA_BIT_MASK(32))
		return -ENOSPC;

	spin_lock_irqsave(&dma_pmd_domains_lock, flags);

	if (win->size)				/* another CPU got here first */
		goto out;

	/* Declined earlier; only DMA_PMD_IDX_NONE means "not tried yet". */
	if (win->domain_idx != DMA_PMD_IDX_NONE) {
		ret = win->domain_idx == DMA_PMD_IDX_NOHUGE ? -EOPNOTSUPP : -ENOSPC;
		goto out;
	}

	if (!(domain->pgsize_bitmap & PMD_SIZE)) {
		dev_warn_ratelimited(dev,
				     "dma_pmd: domain lacks a %luMB page size, not pooling\n",
				     (unsigned long)(PMD_SIZE >> 20));
		ret = -EOPNOTSUPP;
		goto decline_nohuge;
	}

	for (idx = 0; idx < DMA_PMD_MAX_DOMAINS; idx++) {
		if (!dma_pmd_domains[idx].domain && !dma_pmd_domains[idx].draining)
			break;
	}
	if (idx == DMA_PMD_MAX_DOMAINS) {
		pr_warn_ratelimited("dma_pmd: all %d domain indices in use, not pooling for domain %p\n",
				    DMA_PMD_MAX_DOMAINS, domain);
		ret = -ENOSPC;
		goto decline;
	}

	base = dma_pmd_dma_window_alloc(dev, domain, &size);
	if (!base) {
		dev_warn_ratelimited(dev,
				     "dma_pmd: no free %llu GB IOVA range for a DMA_PMD window\n",
				     size >> 30);
		ret = -ENOSPC;
		goto decline;
	}

	dma_pmd_domains[idx].domain = domain;
	dma_pmd_domains[idx].base = base;
	win->base = base;
	win->domain_idx = idx + DMA_PMD_IDX_FIRST;
	/*
	 * Publish .base and .domain_idx before .size. A non-zero .size is what
	 * tells both the map and the unmap side that the other two are valid;
	 * see dma_pmd_window_owns().
	 */
	smp_store_release(&win->size, size);

	dev_info(dev, "dma_pmd: domain %p index %d window %pad + %llu GB\n",
		 domain, idx, &base, size >> 30);
	goto out;

decline_nohuge:
	win->domain_idx = DMA_PMD_IDX_NOHUGE;
	goto out;

decline:
	win->domain_idx = DMA_PMD_IDX_NOSPACE;
out:
	spin_unlock_irqrestore(&dma_pmd_domains_lock, flags);
	return ret;
}

/**
 * dma_pmd_dma_map_phys - Derive the IOVA of a pool address, mapping if needed
 * @dev: Device performing DMA
 * @domain: @dev's DMA domain
 * @win: @domain's DMA_PMD window, from its DMA cookie
 * @phys: Physical address within a DMA_PMD page
 * @size: Mapping size requested
 * @prot: IOMMU protection flags requested by the caller
 * @dma_mask: Device DMA mask
 *
 * The IOVA is not allocated or cached: it is @win->base plus the PMD page's
 * physical address, so every block of every PMD page has a fixed
 * address in this domain that both this function and the unmap path can
 * compute. All the PMD page has to remember is whether the PMD leaf PTE has
 * actually been installed here, which is one bit in @meta->domains_mapped.
 *
 * The PMD page is mapped with IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE so that
 * blocks can be used in any DMA direction. Callers requesting additional
 * protection flags in @prot fall back to per-buffer mapping.
 *
 * Cost:
 *   - Fast path: O(1) lockless - one bit test and an add. No IOVA tree, no
 *     page table, and no metadata load beyond the bitmask word.
 *   - First use of a PMD page in a domain: one iommu_map() of the whole PMD
 *     page, once, under @meta->map_lock.
 * Locking: Fast path is lockless. Slow path acquires @meta->map_lock
 *          (irqsave), and @dma_pmd_domains_lock on the very first PMD page
 *          mapped through @domain.
 * Frequency: Very high (called on every dma_map_page / dma_map_single for
 *            buffers allocated from an dma_pmd_pool).
 *
 * Return: Mapped IOVA, or DMA_MAPPING_ERROR on failure.
 */
dma_addr_t dma_pmd_dma_map_phys(struct device *dev, struct iommu_domain *domain,
				struct dma_pmd_window *win, phys_addr_t phys,
				size_t size, int prot, u64 dma_mask)
{
	int block_prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE;
	unsigned long pfn = phys >> PAGE_SHIFT;
	struct dma_pmd_meta *meta;
	unsigned int domain_idx;
	unsigned long flags;
	dma_addr_t iova;
	u64 win_size;
	int ret;

	if (unlikely((prot & ~block_prot) || !dev_is_dma_coherent(dev) ||
		     !dma_is_pmd_page(pfn) || (phys & (PMD_SIZE - 1)) + size > PMD_SIZE))
		return DMA_MAPPING_ERROR;

	meta = dma_pmd_meta_of_pfn(pfn);

	/* Acquire pairs with the .size release in dma_pmd_window_assign(). */
	win_size = smp_load_acquire(&win->size);

	if (unlikely(!win_size)) {
		u16 win_idx = READ_ONCE(win->domain_idx);

		/*
		 * A declined domain stays declined, so the answer is cached in
		 * @domain_idx and read without @dma_pmd_domains_lock. Otherwise
		 * every map through such a domain would queue on a global
		 * spinlock only to be told no.
		 */
		if (unlikely(win_idx == DMA_PMD_IDX_NOHUGE))
			ret = -EOPNOTSUPP;
		else if (unlikely(win_idx == DMA_PMD_IDX_NOSPACE))
			ret = -ENOSPC;
		else
			ret = dma_pmd_window_assign(dev, domain, win);

		if (ret)
			return DMA_MAPPING_ERROR;
		win_size = READ_ONCE(win->size);
	}

	iova = dma_pmd_window_iova(win, phys);

	/*
	 * The window was sized against the first device to pool through this
	 * domain (and may have been clamped to the domain aperture). Mirror
	 * __iommu_dma_map() and bound by the window size, bus limit, and
	 * device mask.
	 */
	if (unlikely(iova - win->base + size > win_size ||
		     iova + size - 1 > min_not_zero(dma_mask, dev->bus_dma_limit)))
		return DMA_MAPPING_ERROR;

	domain_idx = win->domain_idx - DMA_PMD_IDX_FIRST;
	if (likely(test_bit(domain_idx, &meta->domains_mapped)))
		return iova;

	/* First use of this PMD page in this domain: install the leaf PTE. */
	spin_lock_irqsave(&meta->map_lock, flags);
	if (test_bit(domain_idx, &meta->domains_mapped)) {
		/* raced, someone else mapped */
		ret = 0;
	} else {
		ret = iommu_map(domain,
				dma_pmd_window_iova(win, dma_pmd_meta_to_phys(meta)),
				dma_pmd_meta_to_phys(meta), PMD_SIZE,
				block_prot, GFP_ATOMIC);
		if (!ret) {
			/*
			 * A lockless reader that sees this bit skips the
			 * iommu_map() above and hands the IOVA straight to the
			 * device, so the leaf PTE has to be visible first.
			 * set_bit() carries no ordering of its own on a weakly
			 * ordered machine; the barrier is a no-op on x86,
			 * where the LOCK prefix already provides it.
			 */
			smp_mb__before_atomic();
			set_bit(domain_idx, &meta->domains_mapped);
		}
	}
	spin_unlock_irqrestore(&meta->map_lock, flags);

	if (unlikely(ret))
		return DMA_MAPPING_ERROR;

	return iova;
}
