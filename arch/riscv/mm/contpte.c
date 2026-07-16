// SPDX-License-Identifier: GPL-2.0-only

#include <linux/align.h>
#include <linux/cpufeature.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/page_table_check.h>
#include <linux/pgtable.h>

#include <asm/tlbflush.h>

static inline bool napot_hw_supported(void)
{
	return riscv_has_extension_unlikely(RISCV_ISA_EXT_SVNAPOT);
}

static inline bool mm_is_user(struct mm_struct *mm)
{
	if (unlikely(mm_is_efi(mm)))
		return false;

	return mm != &init_mm;
}

static inline unsigned int napotpte_order(void)
{
	return NAPOT_CONT64KB_ORDER;
}

static inline unsigned long napotpte_size(void)
{
	return napot_cont_size(napotpte_order());
}

static inline unsigned int napotpte_pte_num(void)
{
	return napot_pte_num(napotpte_order());
}

static inline unsigned long napot_align_addr(unsigned long addr)
{
	return ALIGN_DOWN(addr, napotpte_size());
}

static inline pte_t *napot_align_ptep(pte_t *ptep)
{
	return PTR_ALIGN_DOWN(ptep, napotpte_pte_num() * sizeof(*ptep));
}

static inline pte_t pte_mask_ad(pte_t pte)
{
	return pte_mkold(pte_mkclean(pte));
}

void __napotpte_try_unfold(struct mm_struct *mm, unsigned long addr,
			   pte_t *ptep, pte_t pte);

static inline unsigned long pte_protval_no_pfn_no_napot(pte_t pte)
{
	return (pte_val(pte) & ~_PAGE_PFN_MASK) & ~_PAGE_NAPOT;
}

static inline pte_t napotpte_subpte(pte_t *ptep, pte_t pte)
{
	unsigned long pfn;
	pgprot_t prot;

	if (!pte_present_napot(pte))
		return pte;

	pfn = pte_pfn(pte) + (ptep - napot_align_ptep(ptep));
	prot = __pgprot(pte_protval_no_pfn_no_napot(pte));

	return pfn_pte(pfn, prot);
}

static inline pte_t __napot_ptep_get_and_clear(struct mm_struct *mm,
					       unsigned long addr, pte_t *ptep)
{
	pte_t pte;

	pte = __pte(atomic_long_xchg((atomic_long_t *)ptep, 0));
	page_table_check_pte_clear(mm, addr, pte_mknonnapot(pte, addr));

	return pte;
}

static void napotpte_convert(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep, pte_t target)
{
	unsigned long start_addr, end, ptent_addr;
	pte_t *start_ptep;
	pte_t ptent, pte;
	unsigned int i, nr;

	start_addr = napot_align_addr(addr);
	start_ptep = napot_align_ptep(ptep);
	nr = napotpte_pte_num();
	end = start_addr + napotpte_size();

	for (i = 0; i < nr; i++) {
		ptent_addr = start_addr + i * PAGE_SIZE;
		ptent = __napot_ptep_get_and_clear(mm, ptent_addr,
						   start_ptep + i);
		if (pte_dirty(ptent))
			target = riscv_pte_mkhwdirty(target);
		if (pte_young(ptent))
			target = pte_mkyoung(target);
	}

	flush_tlb_mm_range(mm, start_addr, end, PAGE_SIZE);

	page_table_check_ptes_set(mm, start_addr, start_ptep, target, nr);
	if (pte_napot(target)) {
		for (i = 0; i < nr; i++)
			__set_pte_at(mm, start_ptep + i, target);
		return;
	}

	for (i = 0; i < nr; i++) {
		pte = pfn_pte(pte_pfn(target) + i,
			      __pgprot(pte_protval_no_pfn_no_napot(target)));
		if (pte_dirty(target))
			pte = riscv_pte_mkhwdirty(pte);
		if (pte_young(target))
			pte = pte_mkyoung(pte);
		__set_pte_at(mm, start_ptep + i, pte);
	}
}

static inline bool napotpte_is_consistent(pte_t pte, pte_t orig_pte)
{
	return pte_present_napot(pte) &&
	       pte_val(pte_mask_ad(pte)) == pte_val(pte_mask_ad(orig_pte));
}

void __napotpte_try_fold(struct mm_struct *mm, unsigned long addr,
			 pte_t *ptep, pte_t pte)
{
	struct page *page;
	struct folio *folio;
	unsigned long folio_start, folio_end;
	unsigned long cont_start, cont_end;
	unsigned long pfn;
	pgprot_t prot;
	pte_t expected, cur;
	pte_t *start;
	unsigned int i, nr;

	if (!napot_hw_supported() || !mm_is_user(mm))
		return;

	if (!pte_present(pte) || pte_napot(pte) || pte_special(pte))
		return;

	/*
	 * Driver __GFP_COMP pages inserted by vm_insert_page() have valid
	 * compound metadata. Fold them only after verifying the whole supported
	 * Svnapot range is identical and contiguous.
	 */
	page = pte_page(pte);
	folio = page_folio(page);
	folio_start = addr - (page - &folio->page) * PAGE_SIZE;
	folio_end = folio_start + folio_nr_pages(folio) * PAGE_SIZE;
	cont_start = napot_align_addr(addr);
	cont_end = cont_start + napotpte_size();
	if (folio_start > cont_start || folio_end < cont_end)
		return;

	nr = napotpte_pte_num();
	start = napot_align_ptep(ptep);

	pfn = ALIGN_DOWN(pte_pfn(pte), nr);
	prot = pte_pgprot(pte_mask_ad(pte));
	expected = pfn_pte(pfn, prot);

	/*
	 * The caller must hold the PTL across validation and conversion.
	 * Software PTE updates are serialized by the PTL; hardware A/D updates
	 * may race and are re-read and folded into target by napotpte_convert().
	 */
	for (i = 0; i < nr; i++) {
		cur = READ_ONCE(start[i]);
		if (pte_val(pte_mask_ad(cur)) != pte_val(expected))
			return;
		pte_val(expected) += 1UL << _PAGE_PFN_SHIFT;
	}

	expected = pte_mknapot(pfn_pte(pfn, prot), napotpte_order());
	napotpte_convert(mm, addr, ptep, expected);
}
EXPORT_SYMBOL(__napotpte_try_fold);

void __napotpte_try_unfold(struct mm_struct *mm, unsigned long addr,
			   pte_t *ptep, pte_t pte)
{
	pte_t target;
	pgprot_t prot;

	if (!napot_hw_supported() || !mm_is_user(mm) ||
	    !pte_present_napot(pte))
		return;

	prot = __pgprot(pte_protval_no_pfn_no_napot(pte));
	target = pfn_pte(pte_pfn(pte), prot);

	napotpte_convert(mm, addr, ptep, target);
}
EXPORT_SYMBOL(__napotpte_try_unfold);

pte_t napotpte_ptep_get(pte_t *ptep, pte_t orig_pte)
{
	pte_t pte, cur;
	pte_t *start;
	unsigned int i, nr;

	if (!napot_hw_supported() || !pte_present_napot(orig_pte))
		return orig_pte;

	pte = orig_pte;
	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();

	for (i = 0; i < nr; i++) {
		cur = READ_ONCE(start[i]);
		if (!napotpte_is_consistent(cur, orig_pte))
			return napotpte_subpte(ptep, orig_pte);
		if (pte_dirty(cur))
			pte = riscv_pte_mkhwdirty(pte);
		if (pte_young(cur))
			pte = pte_mkyoung(pte);
	}

	return napotpte_subpte(ptep, pte);
}
EXPORT_SYMBOL(napotpte_ptep_get);

pte_t napotpte_ptep_get_lockless(pte_t *orig_ptep)
{
	pte_t orig_pte, pte;
	pte_t *ptep;
	unsigned int i, nr;

	if (!napot_hw_supported())
		return READ_ONCE(*orig_ptep);

	nr = napotpte_pte_num();

retry:
	orig_pte = READ_ONCE(*orig_ptep);
	if (!pte_present_napot(orig_pte))
		return orig_pte;

	ptep = napot_align_ptep(orig_ptep);

	for (i = 0; i < nr; i++, ptep++) {
		pte = READ_ONCE(*ptep);

		if (!napotpte_is_consistent(pte, orig_pte))
			goto retry;

		if (pte_dirty(pte))
			orig_pte = riscv_pte_mkhwdirty(orig_pte);

		if (pte_young(pte))
			orig_pte = pte_mkyoung(orig_pte);
	}

	return napotpte_subpte(orig_ptep, orig_pte);
}
EXPORT_SYMBOL(napotpte_ptep_get_lockless);
