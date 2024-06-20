// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Derived from arch/powerpc/mm/pgtable.c:
 *   Copyright (C) 2024 Jialong Yang (jialong.yang@shingroup.cn)
 */

#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/hugetlb.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>
#include <asm/hugetlb.h>

#include "mmiotrace_arch.h"

static pte_t *mmiotrace_find_linux_pte(pgd_t *pgdp, unsigned long ea,
			bool *is_thp, unsigned int *hpage_shift)
{
	p4d_t p4d, *p4dp;
	pud_t pud, *pudp;
	pmd_t pmd, *pmdp;
	pte_t *ret_pte;
	hugepd_t *hpdp = NULL;
	unsigned int pdshift;

	if (hpage_shift)
		*hpage_shift = 0;

	if (is_thp)
		*is_thp = false;

	/*
	 * Always operate on the local stack value. This make sure the
	 * value don't get updated by a parallel THP split/collapse,
	 * page fault or a page unmap. The return pte_t * is still not
	 * stable. So should be checked there for above conditions.
	 * Top level is an exception because it is folded into p4d.
	 */
	p4dp = p4d_offset(pgdp, ea);
	p4d  = READ_ONCE(*p4dp);
	pdshift = P4D_SHIFT;

	if (p4d_none(p4d))
		return NULL;

	if (p4d_leaf(p4d)) {
		ret_pte = (pte_t *)p4dp;
		goto out;
	}

	if (is_hugepd(__hugepd(p4d_val(p4d)))) {
		hpdp = (hugepd_t *)&p4d;
		goto out_huge;
	}

	/*
	 * Even if we end up with an unmap, the pgtable will not
	 * be freed, because we do an rcu free and here we are
	 * irq disabled
	 */
	pdshift = PUD_SHIFT;
	pudp = pud_offset(&p4d, ea);
	pud  = READ_ONCE(*pudp);

	if (pud_none(pud))
		return NULL;

	if (pud_leaf(pud)) {
		ret_pte = (pte_t *)pudp;
		goto out;
	}

	if (is_hugepd(__hugepd(pud_val(pud)))) {
		hpdp = (hugepd_t *)&pud;
		goto out_huge;
	}

	pdshift = PMD_SHIFT;
	pmdp = pmd_offset(&pud, ea);
	pmd  = READ_ONCE(*pmdp);

	/*
	 * A hugepage collapse is captured by this condition, see
	 * pmdp_collapse_flush.
	 */
	if (pmd_none(pmd))
		return NULL;

#ifdef CONFIG_PPC_BOOK3S_64
	/*
	 * A hugepage split is captured by this condition, see
	 * pmdp_invalidate.
	 *
	 * Huge page modification can be caught here too.
	 */
	if (pmd_is_serializing(pmd))
		return NULL;
#endif

	if (pmd_trans_huge(pmd) || pmd_devmap(pmd)) {
		if (is_thp)
			*is_thp = true;
		ret_pte = (pte_t *)pmdp;
		goto out;
	}

	if (pmd_leaf(pmd)) {
		ret_pte = (pte_t *)pmdp;
		goto out;
	}

	if (is_hugepd(__hugepd(pmd_val(pmd)))) {
		hpdp = (hugepd_t *)&pmd;
		goto out_huge;
	}

	pdshift = PAGE_SHIFT;

	if (hpage_shift)
		*hpage_shift = pdshift;

	return pte_offset_kernel(&pmd, ea);

out_huge:
	if (!hpdp)
		return NULL;

	ret_pte = hugepte_offset(*hpdp, ea, pdshift);
	pdshift = hugepd_shift(*hpdp);
out:
	if (hpage_shift)
		*hpage_shift = pdshift;
	return ret_pte;
}

pte_t *lookup_address(unsigned long address, unsigned int *shift)
{
	unsigned long flags;

	local_irq_save(flags);
	pte_t *pte = mmiotrace_find_linux_pte(pgd_offset_k(address), address, NULL, shift);

	local_irq_restore(flags);

	return pte;
}
