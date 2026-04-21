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

static inline unsigned long napotpte_mask(void)
{
	return napotpte_size() - 1;
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

static inline pte_t
__napot_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	pte_t pte;

	pte = __pte(atomic_long_xchg((atomic_long_t *)ptep, 0));
	page_table_check_pte_clear(mm, addr, pte);

	return pte;
}

static void napotpte_convert(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep, pte_t target)
{
	unsigned long start_addr, end;
	pte_t *start_ptep;
	pte_t ptent, pte;
	unsigned int i, nr;

	start_addr = napot_align_addr(addr);
	start_ptep = napot_align_ptep(ptep);
	nr = napotpte_pte_num();
	end = start_addr + napotpte_size();

	for (i = 0; i < nr; i++) {
		ptent = __napot_ptep_get_and_clear(mm, start_addr + i * PAGE_SIZE,
						   start_ptep + i);
		if (pte_dirty(ptent))
			target = pte_mkdirty(target);
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
			pte = pte_mkdirty(pte);
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

	if (!napot_hw_supported() || !mm_is_user(mm))
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
			pte = pte_mkdirty(pte);
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

		if (pte_dirty(pte)) {
			orig_pte = pte_mkdirty(orig_pte);
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
					orig_pte = pte_mkdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return napotpte_subpte(orig_ptep, orig_pte);
}
EXPORT_SYMBOL(napotpte_ptep_get_lockless);

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
		boundary = (addr + size) & ~napotpte_mask();
		next = (boundary - 1 < end - 1) ? boundary : end;
		chunk = (next - addr) >> PAGE_SHIFT;

		cur = pfn_pte(pfn, prot);
		if (((addr | next | (pfn << PAGE_SHIFT)) & napotpte_mask()) == 0) {
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

void napotpte_clear_young_dirty_ptes(struct vm_area_struct *vma,
				     unsigned long addr, pte_t *ptep,
				     unsigned int nr, cydp_t flags)
{
	struct mm_struct *mm;
	unsigned long start, end;
	unsigned int total;

	mm = vma->vm_mm;
	if (!napot_hw_supported() || !mm_is_user(mm)) {
		for (;;) {
			if (flags == CYDP_CLEAR_YOUNG)
				__ptep_test_and_clear_young(vma, addr, ptep);
			else
				napotpte_clear_young_dirty_pte(ptep, flags);
			if (--nr == 0)
				break;
			ptep++;
			addr += PAGE_SIZE;
		}
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

bool napotpte_ptep_set_access_flags(struct vm_area_struct *vma,
				    unsigned long address, pte_t *ptep,
				    pte_t entry, int dirty)
{
	pte_t orig_pte, raw_pte, napot_pte;
	pte_t *start;
	pgprot_t prot;
	unsigned long start_addr;
	unsigned int i, nr;
	bool changed;

	raw_pte = READ_ONCE(*ptep);
	if (!napot_hw_supported() || !pte_present_napot(raw_pte))
		return false;

	orig_pte = ptep_get(ptep);
	if (pte_val(orig_pte) == pte_val(entry))
		return false;

	if (pte_write(orig_pte) != pte_write(entry)) {
		__napotpte_try_unfold(vma->vm_mm, address, ptep, raw_pte);
		entry = pte_mknonnapot(entry, address);

		return ptep_set_access_flags(vma, address, ptep, entry, dirty);
	}

	prot = pte_pgprot(entry);
	napot_pte = pfn_pte(pte_pfn(raw_pte), prot);
	napot_pte = pte_mknapot(napot_pte, napotpte_order());

	start = napot_align_ptep(ptep);
	start_addr = napot_align_addr(address);
	nr = napotpte_pte_num();
	changed = false;

	page_table_check_ptes_set(vma->vm_mm, start_addr, start, napot_pte, nr);
	for (i = 0; i < nr; i++) {
		if (!pte_same(READ_ONCE(start[i]), napot_pte)) {
			__set_pte_at(vma->vm_mm, start + i, napot_pte);
			changed = true;
		}
	}

	if (changed)
		flush_tlb_range(vma, start_addr, start_addr + napotpte_size());

	return changed;
}
EXPORT_SYMBOL(napotpte_ptep_set_access_flags);

bool napotpte_ptep_test_and_clear_young(struct vm_area_struct *vma,
					unsigned long address, pte_t *ptep)
{
	pte_t *start;
	unsigned int i, nr;
	bool young;

	if (!napot_hw_supported() || !pte_present_napot(READ_ONCE(*ptep)))
		return false;

	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();
	young = false;

	for (i = 0; i < nr; i++)
		young |= test_and_clear_bit(_PAGE_ACCESSED_OFFSET,
					   &pte_val(start[i]));

	return young;
}
EXPORT_SYMBOL(napotpte_ptep_test_and_clear_young);

bool napotpte_ptep_clear_flush_young(struct vm_area_struct *vma,
				     unsigned long address, pte_t *ptep)
{
	unsigned long start_addr;
	bool young;

	young = napotpte_ptep_test_and_clear_young(vma, address, ptep);
	if (!young)
		return false;

	start_addr = napot_align_addr(address);
	flush_tlb_range(vma, start_addr, start_addr + napotpte_size());

	return true;
}
EXPORT_SYMBOL(napotpte_ptep_clear_flush_young);
