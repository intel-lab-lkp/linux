/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_BOOK3S_64_PGTABLE_4K_H
#define _ASM_POWERPC_BOOK3S_64_PGTABLE_4K_H
/*
 * hash 4k can't share hugetlb and also doesn't support THP
 */
#ifndef __ASSEMBLY__
#ifdef CONFIG_HUGETLB_PAGE
static inline int pmd_huge(pmd_t pmd)
{
	/*
	 * leaf pte for huge page
	 */
	if (radix_enabled())
		return !!(pmd_raw(pmd) & cpu_to_be64(_PAGE_PTE));
	return 0;
}

static inline int pud_huge(pud_t pud)
{
	/*
	 * leaf pte for huge page
	 */
	if (radix_enabled())
		return !!(pud_raw(pud) & cpu_to_be64(_PAGE_PTE));
	return 0;
}

#endif /* CONFIG_HUGETLB_PAGE */

#endif /* __ASSEMBLY__ */

#endif /*_ASM_POWERPC_BOOK3S_64_PGTABLE_4K_H */
