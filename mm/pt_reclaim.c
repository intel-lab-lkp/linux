// SPDX-License-Identifier: GPL-2.0
#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <asm-generic/tlb.h>
#include <asm/pgalloc.h>

#include "internal.h"

/*
 * Locking:
 *  - already held the mmap read lock to traverse the pgtable
 *  - use pmd lock for clearing pmd entry
 *  - use pte lock for checking empty PTE page, and release it after clearing
 *    pmd entry, then we can capture the changed pmd in pte_offset_map_lock()
 *    etc after holding this pte lock. Thanks to this, we don't need to hold the
 *    rmap-related locks.
 *  - users of pte_offset_map_lock() etc all expect the PTE page to be stable by
 *    using rcu lock, so PTE pages should be freed by RCU.
 */
static int reclaim_pgtables_pmd_entry(pmd_t *pmd, unsigned long addr,
				      unsigned long next, struct mm_walk *walk)
{
	struct mm_struct *mm = walk->mm;
	struct mmu_gather *tlb = walk->private;
	pte_t *start_pte, *pte;
	pmd_t pmdval;
	spinlock_t *pml = NULL, *ptl;
	int i;

	start_pte = pte_offset_map_nolock(mm, pmd, &pmdval, addr, &ptl);
	if (!start_pte)
		return 0;

	pml = pmd_lock(mm, pmd);
	if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	if (unlikely(!pmd_same(pmdval, pmdp_get_lockless(pmd))))
		goto out_ptl;

	/* Check if it is empty PTE page */
	for (i = 0, pte = start_pte; i < PTRS_PER_PTE; i++, pte++) {
		if (!pte_none(ptep_get(pte)))
			goto out_ptl;
	}
	pte_unmap(start_pte);

	pmd_clear(pmd);
	if (ptl != pml)
		spin_unlock(ptl);
	spin_unlock(pml);

	/*
	 * NOTE:
	 *   In order to reuse mmu_gather to batch flush tlb and free PTE pages,
	 *   here tlb is not flushed before pmd lock is unlocked. This may
	 *   result in the following two situations:
	 *
	 *   1) Userland can trigger page fault and fill a huge page, which will
	 *      cause the existence of small size TLB and huge TLB for the same
	 *      address.
	 *
	 *   2) Userland can also trigger page fault and fill a PTE page, which
	 *      will cause the existence of two small size TLBs, but the PTE
	 *      page they map are different.
	 *
	 * Some CPUs do not allow these, to solve this, we can define
	 * arch_flush_tlb_before_set_{huge|pte}_page to detect this case and
	 * flush TLB before filling a huge page or a PTE page in page fault
	 * path.
	 */
	pte_free_tlb(tlb, pmd_pgtable(pmdval), addr);
	mm_dec_nr_ptes(mm);

	return 0;

out_ptl:
	pte_unmap_unlock(start_pte, ptl);
	if (pml != ptl)
		spin_unlock(pml);

	return 0;
}

static const struct mm_walk_ops reclaim_pgtables_walk_ops = {
	.pmd_entry = reclaim_pgtables_pmd_entry,
	.walk_lock = PGWALK_RDLOCK,
};

void try_to_reclaim_pgtables(struct mmu_gather *tlb, struct vm_area_struct *vma,
			     unsigned long start_addr, unsigned long end_addr,
			     struct zap_details *details)
{
	unsigned long start = max(vma->vm_start, start_addr);
	unsigned long end;

	if (start >= vma->vm_end)
		return;
	end = min(vma->vm_end, end_addr);
	if (end <= vma->vm_start)
		return;

	/* Skip hugetlb case  */
	if (is_vm_hugetlb_page(vma))
		return;

	/* Leave this to the THP path to handle */
	if (vma->vm_flags & VM_HUGEPAGE)
		return;

	/* userfaultfd_wp case may reinstall the pte entry, also skip */
	if (userfaultfd_wp(vma))
		return;

	/*
	 * For private file mapping, the COW-ed page is an anon page, and it
	 * will not be zapped. For simplicity, skip the all writable private
	 * file mapping cases.
	 */
	if (details && !vma_is_anonymous(vma) &&
	    !(vma->vm_flags & VM_MAYSHARE) &&
	    (vma->vm_flags & VM_WRITE))
		return;

	start = ALIGN(start, PMD_SIZE);
	end = ALIGN_DOWN(end, PMD_SIZE);
	if (end - start < PMD_SIZE)
		return;

	walk_page_range_vma(vma, start, end, &reclaim_pgtables_walk_ops, tlb);
}
