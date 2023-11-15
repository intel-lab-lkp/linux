// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <asm/tlbflush.h>

static void ptep_clear_flush_range(struct mm_struct *mm, unsigned long addr,
				pte_t *ptep, int nr)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long start_addr = addr;
	int i;

	for (i = 0; i < nr; i++, ptep++, addr += PAGE_SIZE)
		__pte_clear(mm, addr, ptep);

	__flush_tlb_range(&vma, start_addr, addr, PAGE_SIZE, true, 3);
}

static bool ptep_any_valid(pte_t *ptep, int nr)
{
	int i;

	for (i = 0; i < nr; i++, ptep++) {
		if (pte_valid(__ptep_get(ptep)))
			return true;
	}

	return false;
}

static void contpte_fold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte, bool fold)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long start_addr;
	pte_t *start_ptep;
	int i;

	start_ptep = ptep = contpte_align_down(ptep);
	start_addr = addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);
	pte = pfn_pte(ALIGN_DOWN(pte_pfn(pte), CONT_PTES), pte_pgprot(pte));
	pte = fold ? pte_mkcont(pte) : pte_mknoncont(pte);

	for (i = 0; i < CONT_PTES; i++, ptep++, addr += PAGE_SIZE) {
		pte_t ptent = __ptep_get_and_clear(mm, addr, ptep);

		if (pte_dirty(ptent))
			pte = pte_mkdirty(pte);

		if (pte_young(ptent))
			pte = pte_mkyoung(pte);
	}

	__flush_tlb_range(&vma, start_addr, addr, PAGE_SIZE, true, 3);

	__set_ptes(mm, start_addr, start_ptep, pte, CONT_PTES);
}

void __contpte_try_fold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the virtual and pysical addresses are
	 * correctly aligned for a contpte mapping in contpte_try_fold() so the
	 * remaining checks are to ensure that the contpte range is fully
	 * covered by a single folio, and ensure that all the ptes are valid
	 * with contiguous PFNs and matching prots. We ignore the state of the
	 * access and dirty bits for the purpose of deciding if its a contiguous
	 * range; the folding process will generate a single contpte entry which
	 * has a single access and dirty bit. Those 2 bits are the logical OR of
	 * their respective bits in the constituent pte entries. In order to
	 * ensure the contpte range is covered by a single folio, we must
	 * recover the folio from the pfn, but special mappings don't have a
	 * folio backing them. Fortunately contpte_try_fold() already checked
	 * that the pte is not special - we never try to fold special mappings.
	 * Note we can't use vm_normal_page() for this since we don't have the
	 * vma.
	 */

	struct page *page = pte_page(pte);
	struct folio *folio = page_folio(page);
	unsigned long folio_saddr = addr - (page - &folio->page) * PAGE_SIZE;
	unsigned long folio_eaddr = folio_saddr + folio_nr_pages(folio) * PAGE_SIZE;
	unsigned long cont_saddr = ALIGN_DOWN(addr, CONT_PTE_SIZE);
	unsigned long cont_eaddr = cont_saddr + CONT_PTE_SIZE;
	unsigned long pfn;
	pgprot_t prot;
	pte_t subpte;
	pte_t *orig_ptep;
	int i;

	if (folio_saddr > cont_saddr || folio_eaddr < cont_eaddr)
		return;

	pfn = pte_pfn(pte) - ((addr - cont_saddr) >> PAGE_SHIFT);
	prot = pte_pgprot(pte_mkold(pte_mkclean(pte)));
	orig_ptep = ptep;
	ptep = contpte_align_down(ptep);

	for (i = 0; i < CONT_PTES; i++, ptep++, pfn++) {
		subpte = __ptep_get(ptep);
		subpte = pte_mkold(pte_mkclean(subpte));

		if (!pte_valid(subpte) ||
		    pte_pfn(subpte) != pfn ||
		    pgprot_val(pte_pgprot(subpte)) != pgprot_val(prot))
			return;
	}

	contpte_fold(mm, addr, orig_ptep, pte, true);
}
EXPORT_SYMBOL(__contpte_try_fold);

void __contpte_try_unfold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the ptes are contiguous in
	 * contpte_try_unfold(), so we can unfold unconditionally here.
	 */

	contpte_fold(mm, addr, ptep, pte, false);
}
EXPORT_SYMBOL(__contpte_try_unfold);

pte_t contpte_ptep_get(pte_t *ptep, pte_t orig_pte)
{
	/*
	 * Gather access/dirty bits, which may be populated in any of the ptes
	 * of the contig range. We are guarranteed to be holding the PTL, so any
	 * contiguous range cannot be unfolded or otherwise modified under our
	 * feet.
	 */

	pte_t pte;
	int i;

	ptep = contpte_align_down(ptep);

	for (i = 0; i < CONT_PTES; i++, ptep++) {
		pte = __ptep_get(ptep);

		/*
		 * Deal with the partial contpte_ptep_get_and_clear_full() case,
		 * where some of the ptes in the range may be cleared but others
		 * are still to do. See contpte_ptep_get_and_clear_full().
		 */
		if (pte_val(pte) == 0)
			continue;

		if (pte_dirty(pte))
			orig_pte = pte_mkdirty(orig_pte);

		if (pte_young(pte))
			orig_pte = pte_mkyoung(orig_pte);
	}

	return orig_pte;
}
EXPORT_SYMBOL(contpte_ptep_get);

pte_t contpte_ptep_get_lockless(pte_t *orig_ptep)
{
	/*
	 * Gather access/dirty bits, which may be populated in any of the ptes
	 * of the contig range. We may not be holding the PTL, so any contiguous
	 * range may be unfolded/modified/refolded under our feet. Therefore we
	 * ensure we read a _consistent_ contpte range by checking that all ptes
	 * in the range are valid and have CONT_PTE set, that all pfns are
	 * contiguous and that all pgprots are the same (ignoring access/dirty).
	 * If we find a pte that is not consistent, then we must be racing with
	 * an update so start again. If the target pte does not have CONT_PTE
	 * set then that is considered consistent on its own because it is not
	 * part of a contpte range.
	 */

	pte_t orig_pte;
	pgprot_t orig_prot;
	pte_t *ptep;
	unsigned long pfn;
	pte_t pte;
	pgprot_t prot;
	int i;

retry:
	orig_pte = __ptep_get(orig_ptep);

	if (!pte_valid_cont(orig_pte))
		return orig_pte;

	orig_prot = pte_pgprot(pte_mkold(pte_mkclean(orig_pte)));
	ptep = contpte_align_down(orig_ptep);
	pfn = pte_pfn(orig_pte) - (orig_ptep - ptep);

	for (i = 0; i < CONT_PTES; i++, ptep++, pfn++) {
		pte = __ptep_get(ptep);
		prot = pte_pgprot(pte_mkold(pte_mkclean(pte)));

		if (!pte_valid_cont(pte) ||
		   pte_pfn(pte) != pfn ||
		   pgprot_val(prot) != pgprot_val(orig_prot))
			goto retry;

		if (pte_dirty(pte))
			orig_pte = pte_mkdirty(orig_pte);

		if (pte_young(pte))
			orig_pte = pte_mkyoung(orig_pte);
	}

	return orig_pte;
}
EXPORT_SYMBOL(contpte_ptep_get_lockless);

void contpte_set_ptes(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned long next;
	unsigned long end = addr + (nr << PAGE_SHIFT);
	unsigned long pfn = pte_pfn(pte);
	pgprot_t prot = pte_pgprot(pte);
	pte_t orig_pte;

	do {
		next = pte_cont_addr_end(addr, end);
		nr = (next - addr) >> PAGE_SHIFT;
		pte = pfn_pte(pfn, prot);

		if (((addr | next | (pfn << PAGE_SHIFT)) & ~CONT_PTE_MASK) == 0)
			pte = pte_mkcont(pte);
		else
			pte = pte_mknoncont(pte);

		/*
		 * If operating on a partial contiguous range then we must first
		 * unfold the contiguous range if it was previously folded.
		 * Otherwise we could end up with overlapping tlb entries.
		 */
		if (nr != CONT_PTES)
			contpte_try_unfold(mm, addr, ptep, __ptep_get(ptep));

		/*
		 * If we are replacing ptes that were contiguous or if the new
		 * ptes are contiguous and any of the ptes being replaced are
		 * valid, we need to clear and flush the range to prevent
		 * overlapping tlb entries.
		 */
		orig_pte = __ptep_get(ptep);
		if (pte_valid_cont(orig_pte) ||
		    (pte_cont(pte) && ptep_any_valid(ptep, nr)))
			ptep_clear_flush_range(mm, addr, ptep, nr);

		__set_ptes(mm, addr, ptep, pte, nr);

		addr = next;
		ptep += nr;
		pfn += nr;

	} while (addr != end);
}
EXPORT_SYMBOL(contpte_set_ptes);

pte_t contpte_ptep_get_and_clear_full(struct mm_struct *mm,
					unsigned long addr, pte_t *ptep)
{
	/*
	 * When doing a full address space teardown, we can avoid unfolding the
	 * contiguous range, and therefore avoid the associated tlbi. Instead,
	 * just get and clear the pte. The caller is promising to call us for
	 * every pte, so every pte in the range will be cleared by the time the
	 * tlbi is issued.
	 *
	 * This approach is not perfect though, as for the duration between
	 * returning from the first call to ptep_get_and_clear_full() and making
	 * the final call, the contpte block in an intermediate state, where
	 * some ptes are cleared and others are still set with the PTE_CONT bit.
	 * If any other APIs are called for the ptes in the contpte block during
	 * that time, we have to be very careful. The core code currently
	 * interleaves calls to ptep_get_and_clear_full() with ptep_get() and so
	 * ptep_get() must be careful to ignore the cleared entries when
	 * accumulating the access and dirty bits - the same goes for
	 * ptep_get_lockless(). The only other calls we might resonably expect
	 * are to set markers in the previously cleared ptes. (We shouldn't see
	 * valid entries being set until after the tlbi, at which point we are
	 * no longer in the intermediate state). Since markers are not valid,
	 * this is safe; set_ptes() will see the old, invalid entry and will not
	 * attempt to unfold. And the new pte is also invalid so it won't
	 * attempt to fold. We shouldn't see this for the 'full' case anyway.
	 *
	 * The last remaining issue is returning the access/dirty bits. That
	 * info could be present in any of the ptes in the contpte block.
	 * ptep_get() will gather those bits from across the contpte block. We
	 * don't bother doing that here, because we know that the information is
	 * used by the core-mm to mark the underlying folio as accessed/dirty.
	 * And since the same folio must be underpinning the whole block (that
	 * was a requirement for folding in the first place), that information
	 * will make it to the folio eventually once all the ptes have been
	 * cleared. This approach means we don't have to play games with
	 * accumulating and storing the bits. It does mean that any interleaved
	 * calls to ptep_get() may lack correct access/dirty information if we
	 * have already cleared the pte that happened to store it. The core code
	 * does not rely on this though.
	 */

	return __ptep_get_and_clear(mm, addr, ptep);
}
EXPORT_SYMBOL(contpte_ptep_get_and_clear_full);

int contpte_ptep_test_and_clear_young(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep)
{
	/*
	 * ptep_clear_flush_young() technically requires us to clear the access
	 * flag for a _single_ pte. However, the core-mm code actually tracks
	 * access/dirty per folio, not per page. And since we only create a
	 * contig range when the range is covered by a single folio, we can get
	 * away with clearing young for the whole contig range here, so we avoid
	 * having to unfold.
	 */

	int i;
	int young = 0;

	ptep = contpte_align_down(ptep);
	addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);

	for (i = 0; i < CONT_PTES; i++, ptep++, addr += PAGE_SIZE)
		young |= __ptep_test_and_clear_young(vma, addr, ptep);

	return young;
}
EXPORT_SYMBOL(contpte_ptep_test_and_clear_young);

int contpte_ptep_clear_flush_young(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep)
{
	int young;

	young = contpte_ptep_test_and_clear_young(vma, addr, ptep);

	if (young) {
		/*
		 * See comment in __ptep_clear_flush_young(); same rationale for
		 * eliding the trailing DSB applies here.
		 */
		addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);
		__flush_tlb_range_nosync(vma, addr, addr + CONT_PTE_SIZE,
					 PAGE_SIZE, true, 3);
	}

	return young;
}
EXPORT_SYMBOL(contpte_ptep_clear_flush_young);

void contpte_set_wrprotects(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, unsigned int nr)
{
	unsigned long next;
	unsigned long end = addr + (nr << PAGE_SHIFT);

	do {
		next = pte_cont_addr_end(addr, end);
		nr = (next - addr) >> PAGE_SHIFT;

		/*
		 * If wrprotecting an entire contig range, we can avoid
		 * unfolding. Just set wrprotect and wait for the later
		 * mmu_gather flush to invalidate the tlb. Until the flush, the
		 * page may or may not be wrprotected. After the flush, it is
		 * guarranteed wrprotected. If its a partial range though, we
		 * must unfold, because we can't have a case where CONT_PTE is
		 * set but wrprotect applies to a subset of the PTEs; this would
		 * cause it to continue to be unpredictable after the flush.
		 */
		if (nr != CONT_PTES)
			contpte_try_unfold(mm, addr, ptep, __ptep_get(ptep));

		__ptep_set_wrprotects(mm, addr, ptep, nr);

		addr = next;
		ptep += nr;

		/*
		 * If applying to a partial contig range, the change could have
		 * made the range foldable. Use the last pte in the range we
		 * just set for comparison, since contpte_try_fold() only
		 * triggers when acting on the last pte in the contig range.
		 */
		if (nr != CONT_PTES)
			contpte_try_fold(mm, addr - PAGE_SIZE, ptep - 1,
					 __ptep_get(ptep - 1));

	} while (addr != end);
}
EXPORT_SYMBOL(contpte_set_wrprotects);

int contpte_ptep_set_access_flags(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep,
					pte_t entry, int dirty)
{
	pte_t orig_pte;
	int i;
	unsigned long start_addr;

	/*
	 * Gather the access/dirty bits for the contiguous range. If nothing has
	 * changed, its a noop.
	 */
	orig_pte = ptep_get(ptep);
	if (pte_val(orig_pte) == pte_val(entry))
		return 0;

	/*
	 * We can fix up access/dirty bits without having to unfold/fold the
	 * contig range. But if the write bit is changing, we need to go through
	 * the full unfold/fold cycle.
	 */
	if (pte_write(orig_pte) == pte_write(entry)) {
		/*
		 * For HW access management, we technically only need to update
		 * the flag on a single pte in the range. But for SW access
		 * management, we need to update all the ptes to prevent extra
		 * faults. Avoid per-page tlb flush in __ptep_set_access_flags()
		 * and instead flush the whole range at the end.
		 */
		ptep = contpte_align_down(ptep);
		start_addr = addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);

		for (i = 0; i < CONT_PTES; i++, ptep++, addr += PAGE_SIZE)
			__ptep_set_access_flags(vma, addr, ptep, entry, 0);

		if (dirty)
			__flush_tlb_range(vma, start_addr, addr,
							PAGE_SIZE, true, 3);
	} else {
		__contpte_try_unfold(vma->vm_mm, addr, ptep, orig_pte);
		__ptep_set_access_flags(vma, addr, ptep, entry, dirty);
		contpte_try_fold(vma->vm_mm, addr, ptep, entry);
	}

	return 1;
}
EXPORT_SYMBOL(contpte_ptep_set_access_flags);
