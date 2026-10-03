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
	phys_addr_t phys = dma_pmd_meta_to_phys(meta);
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

		iommu_unmap(domain, base + phys, PMD_SIZE);
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
