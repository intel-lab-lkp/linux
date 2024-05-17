/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H
#define _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H

#define PAGE_SHIFT_8M		23

static inline void flush_hugetlb_page(struct vm_area_struct *vma,
				      unsigned long vmaddr)
{
	flush_tlb_page(vma, vmaddr);
}

static inline int check_and_get_huge_psize(int shift)
{
	return shift_to_mmu_psize(shift);
}

#define __HAVE_ARCH_HUGE_PTEP_GET
static inline pte_t huge_ptep_get(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	pmd_t *pmdp = (pmd_t *)ptep;

	if (pmdp == pmd_off(mm, ALIGN_DOWN(addr, SZ_8M)))
		ptep = pte_offset_kernel(pmdp, 0);
	return ptep_get(ptep);
}

#define __HAVE_ARCH_HUGE_PTE_CLEAR
static inline void huge_pte_clear(struct mm_struct *mm, unsigned long addr,
				  pte_t *ptep, unsigned long sz)
{
	pmd_t *pmdp = (pmd_t *)ptep;

	if (pmdp == pmd_off(mm, ALIGN_DOWN(addr, SZ_8M))) {
		pte_update(mm, addr, pte_offset_kernel(pmdp, 0), ~0UL, 0, 1);
		pte_update(mm, addr, pte_offset_kernel(pmdp + 1, 0), ~0UL, 0, 1);
	} else {
		pte_update(mm, addr, ptep, ~0UL, 0, 1);
	}
}

#define __HAVE_ARCH_HUGE_PTEP_SET_WRPROTECT
static inline void huge_ptep_set_wrprotect(struct mm_struct *mm,
					   unsigned long addr, pte_t *ptep)
{
	unsigned long clr = ~pte_val(pte_wrprotect(__pte(~0)));
	unsigned long set = pte_val(pte_wrprotect(__pte(0)));

	pmd_t *pmdp = (pmd_t *)ptep;

	if (pmdp == pmd_off(mm, ALIGN_DOWN(addr, SZ_8M))) {
		pte_update(mm, addr, pte_offset_kernel(pmdp, 0), clr, set, 1);
		pte_update(mm, addr, pte_offset_kernel(pmdp + 1, 0), clr, set, 1);
	} else {
		pte_update(mm, addr, ptep, clr, set, 1);
	}
}

#ifdef CONFIG_PPC_4K_PAGES
static inline pte_t arch_make_huge_pte(pte_t entry, unsigned int shift, vm_flags_t flags)
{
	size_t size = 1UL << shift;

	if (size == SZ_16K)
		return __pte(pte_val(entry) | _PAGE_SPS);
	else
		return __pte(pte_val(entry) | _PAGE_SPS | _PAGE_HUGE);
}
#define arch_make_huge_pte arch_make_huge_pte
#endif

#endif /* _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H */
