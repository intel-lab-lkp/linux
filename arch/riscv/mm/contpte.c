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

static inline void napotpte_clear_young_dirty_pte(pte_t *ptep, cydp_t flags)
{
	pte_t old_pte, new_pte;
	unsigned long old_val, new_val;

	do {
		old_pte = READ_ONCE(*ptep);
		new_pte = old_pte;
		if (flags & CYDP_CLEAR_YOUNG)
			new_pte = pte_mkold(new_pte);
		if (flags & CYDP_CLEAR_DIRTY)
			new_pte = pte_mkclean(new_pte);

		old_val = pte_val(old_pte);
		new_val = pte_val(new_pte);
	} while (cmpxchg_relaxed(&pte_val(*ptep), old_val, new_val) != old_val);
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

static void __clear_full_ptes(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned int nr, int full)
{
	for (;;) {
		__ptep_get_and_clear(mm, addr, ptep);
		if (--nr == 0)
			break;
		ptep++;
		addr += PAGE_SIZE;
	}
}

static pte_t __get_and_clear_full_ptes(struct mm_struct *mm,
				       unsigned long addr, pte_t *ptep,
				       unsigned int nr, int full)
{
	pte_t pte, tmp_pte;

	pte = __ptep_get_and_clear(mm, addr, ptep);
	while (--nr) {
		ptep++;
		addr += PAGE_SIZE;
		tmp_pte = __ptep_get_and_clear(mm, addr, ptep);
		if (pte_dirty(tmp_pte))
			pte = riscv_pte_mkhwdirty(pte);
		if (pte_young(tmp_pte))
			pte = pte_mkyoung(pte);
	}

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
		ptent = __ptep_get_and_clear_noptc(start_ptep + i);
		page_table_check_pte_clear(mm, ptent_addr,
					   pte_mknonnapot(ptent, ptent_addr));
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

static inline bool
napotpte_is_batch_consistent(pte_t pte, pte_t batch_pte, fpb_t flags)
{
	return pte_present_napot(pte) &&
	       pte_val(__pte_batch_clear_ignored(pte, flags)) ==
	       pte_val(batch_pte);
}

static inline pte_t
napotpte_normalize_batch_pte(pte_t *ptep, pte_t orig_pte, fpb_t flags)
{
	unsigned long pfn;
	pgprot_t prot;
	unsigned int off;

	if (pte_present_napot(orig_pte))
		return __pte_batch_clear_ignored(orig_pte, flags);

	off = ptep - napot_align_ptep(ptep);
	pfn = pte_pfn(orig_pte) - off;
	prot = __pgprot(pte_protval_no_pfn_no_napot(orig_pte));

	return __pte_batch_clear_ignored(pte_mknapot(pfn_pte(pfn, prot),
					     napotpte_order()), flags);
}

static bool napotpte_all_subptes_same(pte_t *ptep, pte_t expected_pte)
{
	pte_t *start;
	unsigned int i, nr;

	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();

	for (i = 0; i < nr; i++) {
		if (!pte_same(READ_ONCE(start[i]), expected_pte))
			return false;
	}

	return true;
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
	/*
	 * Gather access/dirty bits from the whole NAPOT range for the
	 * ptep_get() view. The returned sub-PTE is built from orig_pte;
	 * neighbouring entries only contribute A/D state. Lockless callers
	 * requiring a self-consistent range must use ptep_get_lockless().
	 */

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
		if (pte_dirty(cur)) {
			pte = riscv_pte_mkhwdirty(pte);
			for (; i < nr; i++) {
				cur = READ_ONCE(start[i]);
				if (pte_young(cur)) {
					pte = pte_mkyoung(pte);
					break;
				}
			}
			break;
		}

		if (pte_young(cur)) {
			pte = pte_mkyoung(pte);
			i++;
			for (; i < nr; i++) {
				cur = READ_ONCE(start[i]);
				if (pte_dirty(cur)) {
					pte = riscv_pte_mkhwdirty(pte);
					break;
				}
			}
			break;
		}
	}

	return napotpte_subpte(ptep, pte);
}
EXPORT_SYMBOL(napotpte_ptep_get);

pte_t napotpte_ptep_get_lockless(pte_t *orig_ptep)
{
	/*
	 * ptep_get_lockless() must return a self-consistent PTE without the
	 * PTL. Recheck that the whole NAPOT range still describes the same
	 * mapping, ignoring A/D bits, and retry if a concurrent update tears
	 * the range while A/D state is being gathered.
	 */
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

		if (pte_dirty(pte)) {
			orig_pte = riscv_pte_mkhwdirty(orig_pte);
			for (; i < nr; i++, ptep++) {
				pte = READ_ONCE(*ptep);

				if (!napotpte_is_consistent(pte, orig_pte))
					goto retry;

				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			for (; i < nr; i++, ptep++) {
				pte = READ_ONCE(*ptep);

				if (!napotpte_is_consistent(pte, orig_pte))
					goto retry;

				if (pte_dirty(pte)) {
					orig_pte = riscv_pte_mkhwdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return napotpte_subpte(orig_ptep, orig_pte);
}
EXPORT_SYMBOL(napotpte_ptep_get_lockless);

unsigned int napotpte_pte_batch_hint_from_pte(pte_t *ptep, pte_t orig_pte,
					      fpb_t flags)
{
	pte_t batch_pte, pte;
	pte_t *start;
	unsigned int i, nr, off;

	if (!napot_hw_supported())
		return 1;

	if (!pte_present_napot(orig_pte) && !pte_present_napot(READ_ONCE(*ptep)))
		return 1;

	/*
	 * @orig_pte may be either the raw NAPOT entry read from the page
	 * table or the logical sub-PTE returned by ptep_get(). In the latter
	 * case, the public view has the NAPOT bit stripped and the PFN adjusted
	 * by the slot offset within the folded block.
	 *
	 * If the caller passes a logical sub-PTE, rebuild the corresponding
	 * block PTE first. Then apply the same folio batch flags as the generic
	 * batching code, and only return a multi-entry hint if every remaining
	 * raw PTE in the folded block still matches.
	 */
	batch_pte = napotpte_normalize_batch_pte(ptep, orig_pte, flags);

	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();
	off = ptep - start;

	for (i = off; i < nr; i++) {
		pte = READ_ONCE(start[i]);
		if (!napotpte_is_batch_consistent(pte, batch_pte, flags))
			return 1;
	}

	return nr - off;
}
EXPORT_SYMBOL(napotpte_pte_batch_hint_from_pte);

static void napotpte_try_unfold_range(struct mm_struct *mm,
				      unsigned long addr, pte_t *ptep,
				      unsigned int nr)
{
	unsigned long next;
	pte_t pte;
	unsigned int chunk;

	while (nr) {
		pte = READ_ONCE(*ptep);
		if (pte_present_napot(pte)) {
			__napotpte_try_unfold(mm, addr, ptep, pte);
			next = napot_align_addr(addr) + napotpte_size();
			chunk = (next - addr) >> PAGE_SHIFT;
		} else {
			chunk = 1;
		}

		if (chunk > nr)
			chunk = nr;

		ptep += chunk;
		addr += chunk * PAGE_SIZE;
		nr -= chunk;
	}
}

static void napotpte_try_unfold_partial(struct mm_struct *mm,
					unsigned long addr, pte_t *ptep,
					unsigned int nr)
{
	pte_t pte;

	if (ptep != napot_align_ptep(ptep) || nr < napotpte_pte_num()) {
		pte = READ_ONCE(*ptep);
		if (pte_present_napot(pte))
			__napotpte_try_unfold(mm, addr, ptep, pte);
	}

	if (ptep + nr != napot_align_ptep(ptep + nr)) {
		unsigned long last_addr;
		pte_t *last_ptep;

		last_addr = addr + PAGE_SIZE * (nr - 1);
		last_ptep = ptep + nr - 1;
		pte = READ_ONCE(*last_ptep);
		if (pte_present_napot(pte))
			__napotpte_try_unfold(mm, last_addr, last_ptep, pte);
	}
}

void napotpte_set_ptes(struct mm_struct *mm, unsigned long addr,
		       pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned long next, end;
	unsigned long pfn, size, boundary;
	pgprot_t prot;
	unsigned int chunk, i;
	pte_t cur;

	if (!napot_hw_supported() || !mm_is_user(mm)) {
		__set_ptes(mm, addr, ptep, pte, nr);
		return;
	}

	size = napotpte_size();
	end = addr + ((unsigned long)nr << PAGE_SHIFT);
	pfn = pte_pfn(pte);
	prot = __pgprot(pte_protval_no_pfn_no_napot(pte));

	do {
		boundary = (addr + size) & ~(size - 1);
		next = (boundary - 1 < end - 1) ? boundary : end;
		chunk = (next - addr) >> PAGE_SHIFT;

		cur = pfn_pte(pfn, prot);
		if (((addr | next | (pfn << PAGE_SHIFT)) & (size - 1)) == 0) {
			cur = pte_mknapot(cur, napotpte_order());
			page_table_check_ptes_set(mm, addr, ptep, cur, chunk);
			for (i = 0; i < chunk; i++)
				__set_pte_at(mm, ptep + i, cur);
		} else {
			__set_ptes(mm, addr, ptep, cur, chunk);
		}

		addr = next;
		ptep += chunk;
		pfn += chunk;
	} while (addr != end);
}
EXPORT_SYMBOL(napotpte_set_ptes);

void napotpte_clear_full_ptes(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned int nr, int full)
{
	if (!napot_hw_supported() || !mm_is_user(mm)) {
		__clear_full_ptes(mm, addr, ptep, nr, full);
		return;
	}

	/*
	 * Svnapot stores identical napot-encoded entries across the whole block
	 * rather than per-page PFNs, so batch zap paths must unfold the covered
	 * range before the generic MM consumes ordinary per-page PTEs.
	 */
	napotpte_try_unfold_range(mm, addr, ptep, nr);
	__clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL(napotpte_clear_full_ptes);

pte_t napotpte_get_and_clear_full_ptes(struct mm_struct *mm,
				       unsigned long addr, pte_t *ptep,
				       unsigned int nr, int full)
{
	if (!napot_hw_supported() || !mm_is_user(mm))
		return __get_and_clear_full_ptes(mm, addr, ptep, nr, full);

	napotpte_try_unfold_range(mm, addr, ptep, nr);

	return __get_and_clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL(napotpte_get_and_clear_full_ptes);

void napotpte_clear_young_dirty_ptes(struct vm_area_struct *vma,
				     unsigned long addr, pte_t *ptep,
				     unsigned int nr, cydp_t flags)
{
	struct mm_struct *mm;
	unsigned long start, end;
	unsigned int total;

	mm = vma->vm_mm;
	if (!napot_hw_supported() || !mm_is_user(mm)) {
		__clear_young_dirty_ptes(vma, addr, ptep, nr, flags);
		return;
	}

	start = addr;
	end = start + nr * PAGE_SIZE;

	if (pte_present_napot(READ_ONCE(*(ptep + nr - 1))))
		end = ALIGN(end, napotpte_size());

	if (pte_present_napot(READ_ONCE(*ptep))) {
		start = napot_align_addr(start);
		ptep = napot_align_ptep(ptep);
	}

	total = (end - start) >> PAGE_SHIFT;
	for (; total; total--, ptep++, start += PAGE_SIZE)
		napotpte_clear_young_dirty_pte(ptep, flags);
}
EXPORT_SYMBOL(napotpte_clear_young_dirty_ptes);

void napotpte_wrprotect_ptes(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep, unsigned int nr)
{
	unsigned int i;

	if (!napot_hw_supported() || !mm_is_user(mm)) {
		for (i = 0; i < nr; i++, ptep++, addr += PAGE_SIZE)
			__ptep_set_wrprotect(mm, addr, ptep);
		return;
	}

	napotpte_try_unfold_partial(mm, addr, ptep, nr);

	for (i = 0; i < nr; i++, ptep++, addr += PAGE_SIZE)
		__ptep_set_wrprotect(mm, addr, ptep);
}
EXPORT_SYMBOL(napotpte_wrprotect_ptes);

int napotpte_ptep_set_access_flags(struct vm_area_struct *vma,
				   unsigned long address, pte_t *ptep,
				   pte_t entry, int dirty)
{
	pte_t raw_pte, napot_pte;
	pte_t *start;
	pgprot_t prot;
	unsigned long start_addr;
	unsigned int i, nr;
	bool changed;

	raw_pte = READ_ONCE(*ptep);
	if (!napot_hw_supported() || !pte_present_napot(raw_pte))
		return 0;

	prot = pte_pgprot(entry);
	napot_pte = pfn_pte(pte_pfn(raw_pte), prot);
	napot_pte = pte_mknapot(napot_pte, napotpte_order());

	if (napotpte_all_subptes_same(ptep, napot_pte))
		return !riscv_has_extension_unlikely(RISCV_ISA_EXT_SVVPTC);

	if (pte_write(raw_pte) != pte_write(napot_pte)) {
		__napotpte_try_unfold(vma->vm_mm, address, ptep, raw_pte);
		entry = pte_mknonnapot(entry, address);

		return __ptep_set_access_flags(vma, address, ptep, entry,
					      dirty);
	}

	start = napot_align_ptep(ptep);
	address = napot_align_addr(address);
	start_addr = address;
	nr = napotpte_pte_num();
	changed = false;

	for (i = 0; i < nr; i++, start++, address += PAGE_SIZE) {
		if (__ptep_set_access_flags(vma, address, start, napot_pte, 0))
			changed = true;
	}

	if (changed)
		flush_tlb_range(vma, start_addr, start_addr + napotpte_size());

	return changed;
}
EXPORT_SYMBOL(napotpte_ptep_set_access_flags);

int napotpte_ptep_test_and_clear_young(struct vm_area_struct *vma,
				       unsigned long address, pte_t *ptep)
{
	pte_t *start;
	unsigned int i, nr;
	int young;

	if (!napot_hw_supported() || !pte_present_napot(READ_ONCE(*ptep)))
		return 0;

	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();
	young = 0;

	for (i = 0; i < nr; i++)
		young |= test_and_clear_bit(_PAGE_ACCESSED_OFFSET,
					   &pte_val(start[i]));

	return young;
}
EXPORT_SYMBOL(napotpte_ptep_test_and_clear_young);

int napotpte_ptep_clear_flush_young(struct vm_area_struct *vma,
				    unsigned long address, pte_t *ptep)
{
	unsigned long start_addr;
	int young;

	young = napotpte_ptep_test_and_clear_young(vma, address, ptep);
	if (!young)
		return 0;

	start_addr = napot_align_addr(address);
	flush_tlb_range(vma, start_addr, start_addr + napotpte_size());

	return young;
}
EXPORT_SYMBOL(napotpte_ptep_clear_flush_young);
