// SPDX-License-Identifier: GPL-2.0-only

#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/pgtable.h>
#include <linux/export.h>

#include <linux/set_memory.h>

/*
 * Serialize cpa() so that we don't allow any other cpu, with stale large tlb
 * entries, to change the page attribute in parallel to some other cpu
 * splitting a large page entry along with changing the attribute.
 */
static DEFINE_SPINLOCK(cpa_lock);

static inline void arch_lock(void)
{
#ifdef CONFIG_X86
	spin_lock(&pgd_lock);
#endif
}

static inline void arch_unlock(void)
{
#ifdef CONFIG_X86
	spin_unlock(&pgd_lock);
#endif
}

unsigned long cpa_addr(struct cpa_data *cpa, unsigned long idx)
{
	if (cpa->flags & CPA_PAGES_ARRAY) {
		struct page *page = cpa->pages[idx];

		if (unlikely(PageHighMem(page)))
			return 0;

		return (unsigned long)page_address(page);
	}

	if (cpa->flags & CPA_ARRAY)
		return cpa->vaddr[idx];

	return *cpa->vaddr + idx * PAGE_SIZE;
}

/*
 * Lookup the page table entry for a virtual address in a specific pgd.
 * Return a pointer to the entry (or NULL if the entry does not exist),
 * the level of the entry, and the effective NX and RW bits of all
 * page table levels.
 */
pte_t *lookup_address_in_pgd_attr(pgd_t *pgdp, unsigned long address,
				  unsigned int *level, bool *nx, bool *rw)
{
	pgd_t pgd = pgdp_get(pgdp);
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;

	*level = PGTABLE_LEVEL_PGD;
	*nx = false;
	*rw = true;

	if (pgd_none(pgd))
		return NULL;

	*level = PGTABLE_LEVEL_P4D;
	*nx |= !pgd_exec(pgd);
	*rw &= !!(pgd_write(pgd));

	p4dp = p4d_offset(pgdp, address);
	p4d = p4dp_get(p4dp);
	if (p4d_none(p4d))
		return NULL;

	if (p4d_leaf(p4d) || !p4d_present(p4d))
		return (pte_t *)p4dp;

	*level = PGTABLE_LEVEL_PUD;
	*nx |= !p4d_exec(p4d);
	*rw &= !!(p4d_write(p4d));

	pudp = pud_offset(p4dp, address);
	pud = pudp_get(pudp);
	if (pud_none(pud))
		return NULL;

	if (pud_leaf(pud) || !pud_present(pud))
		return (pte_t *)pudp;

	*level = PGTABLE_LEVEL_PMD;
	*nx |= !pud_exec(pud);
	*rw &= !!(pud_write(pud));

	pmdp = pmd_offset(pudp, address);
	pmd = pmdp_get(pmdp);
	if (pmd_none(pmd))
		return NULL;

	if (pmd_leaf(pmd) || !pmd_present(pmd))
		return (pte_t *)pmdp;

	*level = PGTABLE_LEVEL_PTE;
	*nx |= !pmd_exec(pmd);
	*rw &= !!(pmd_write(pmd));

	return pte_offset_kernel(pmdp, address);
}

/*
 * Lookup the page table entry for a virtual address in a specific pgd.
 * Return a pointer to the entry and the level of the mapping.
 */
pte_t *lookup_address_in_pgd(pgd_t *pgd, unsigned long address,
			     unsigned int *level)
{
	bool nx, rw;

	return lookup_address_in_pgd_attr(pgd, address, level, &nx, &rw);
}

/*
 * Lookup the page table entry for a virtual address. Return a pointer
 * to the entry and the level of the mapping.
 *
 * Note: the function returns p4d, pud or pmd either when the entry is marked
 * large or when the present bit is not set. Otherwise it returns NULL.
 */
pte_t *lookup_address(unsigned long address, unsigned int *level)
{
	return lookup_address_in_pgd(pgd_offset_k(address), address, level);
}
EXPORT_SYMBOL_GPL(lookup_address);

static pte_t *_lookup_address_cpa(struct cpa_data *cpa, unsigned long address,
			   unsigned int *level, bool *nx, bool *rw)
{
	pgd_t *pgd;

	if (!cpa->pgd)
		pgd = pgd_offset_k(address);
	else
		pgd = cpa->pgd + pgd_index(address);

	return lookup_address_in_pgd_attr(pgd, address, level, nx, rw);
}

static int should_split_large_page(struct cpa_data *cpa,
				   struct cpa_split_data *sd)
{
	enum pgtable_level level = sd->level;
	unsigned long address = sd->address;
	unsigned long numpages, lpaddr;

	if (cpa->force_split)
		return 1;

	sd->psize = pgtable_level_size(level);
	sd->pmask = pgtable_level_mask(level);

	/*
	 * Calculate the number of pages, which fit into this large
	 * page starting at address:
	 */
	lpaddr = (address + sd->psize) & sd->pmask;
	numpages = (lpaddr - address) >> PAGE_SHIFT;
	if (numpages < cpa->numpages)
		cpa->numpages = numpages;

	return arch_should_split_large_page(cpa, sd);
}

static int cpa_handle_large_page(struct cpa_data *cpa, pte_t *kpte,
				  unsigned long address)
{
	struct cpa_split_data sd = {
		.address = address,
		.kpte = kpte,
	};
	int do_split, ret = 1;
	pte_t *tmp;

	spin_unlock(&cpa_lock);
	sd.ptdesc = pagetable_alloc(GFP_KERNEL, 0);
	spin_lock(&cpa_lock);
	if (!sd.ptdesc)
		return -ENOMEM;

	arch_lock();

	/*
	 * Check for races, another CPU might have split this page
	 * up already:
	 */
	tmp = _lookup_address_cpa(cpa, address, &sd.level, &sd.nx, &sd.rw);
	if (tmp != kpte)
		goto out_free_ptdesc;

	/*
	 * Check, whether we can keep the large page intact
	 * and just change the pte:
	 */
	do_split = should_split_large_page(cpa, &sd);
	ret = do_split;
	/*
	 * When the range fits into the existing large page, no split is
	 * required.
	 * should_split_large_page() updated the large page attributes and
	 * cpa->numpages and cpa->pfn.
	 */
	if (do_split <= 0)
		goto out_free_ptdesc;

	/*
	 * We have to split the large page:
	 */
	ret = arch_split_large_page(cpa, &sd);
	if (ret)
		goto out_free_ptdesc;

	arch_unlock();
	return do_split;

out_free_ptdesc:
	arch_unlock();
	pagetable_free(sd.ptdesc);
	return ret;
}

static int __change_page_attr(struct cpa_data *cpa, int primary)
{
	unsigned long address;
	int split_res;
	unsigned int level;
	pte_t *kpte, old_pte;
	bool nx, rw;

	address = cpa_addr(cpa, cpa->curpage);
repeat:
	kpte = _lookup_address_cpa(cpa, address, &level, &nx, &rw);
	if (!kpte)
		return arch_cpa_process_fault(cpa, address, primary);

	old_pte = *kpte;
	if (pte_none(old_pte))
		return arch_cpa_process_fault(cpa, address, primary);

	if (level == PGTABLE_LEVEL_PTE) {
		arch_change_pte(cpa, address, kpte, old_pte, nx, rw);
		cpa->numpages = 1;
		return 0;
	}

	/*
	 * The address maps a large page. Try to keep it, otherwise split it
	 * up and retry on the freshly installed smaller mapping.
	 */
	split_res = cpa_handle_large_page(cpa, kpte, address);
	if (split_res > 0)
		goto repeat;

	return split_res;
}

int __change_page_attr_set_clr(struct cpa_data *cpa, int primary)
{
	unsigned long numpages = cpa->numpages;
	unsigned long rempages = numpages;
	int ret = 0;

	/*
	 * No changes, easy!
	 */
	if (!(pgprot_val(cpa->mask_set) | pgprot_val(cpa->mask_clr)) &&
	    !cpa->force_split)
		return ret;

	while (rempages) {
		/*
		 * Store the remaining nr of pages for the large page
		 * preservation check.
		 */
		cpa->numpages = rempages;
		/* for array changes, we can't use large page */
		if (cpa->flags & (CPA_ARRAY | CPA_PAGES_ARRAY))
			cpa->numpages = 1;

		spin_lock(&cpa_lock);
		ret = __change_page_attr(cpa, primary);
		spin_unlock(&cpa_lock);
		if (ret)
			goto out;

		if (primary && !(cpa->flags & CPA_NO_CHECK_ALIAS) &&
		    cpa_should_update_alias(cpa_addr(cpa, cpa->curpage), cpa->pfn)) {
			ret = arch_cpa_process_alias(cpa);
			if (ret)
				goto out;
		}

		/*
		 * Adjust the number of pages with the result of the
		 * CPA operation. Either a large page has been
		 * preserved or a single page update happened.
		 */
		BUG_ON(cpa->numpages > rempages || !cpa->numpages);
		rempages -= cpa->numpages;
		cpa->curpage += cpa->numpages;
	}

out:
	/* Restore the original numpages */
	cpa->numpages = numpages;
	return ret;
}

int change_page_attr_set_clr(unsigned long *addr, int numpages,
			     pgprot_t mask_set, pgprot_t mask_clr,
			     int force_split, int in_flag,
			     struct page **pages)
{
	struct cpa_data cpa;
	int err;

	memset(&cpa, 0, sizeof(cpa));

	/*
	 * Check, if we are requested to set a not supported
	 * feature.  Clearing non-supported features is OK.
	 */
	mask_set = canon_pgprot(mask_set);

	if (!pgprot_val(mask_set) && !pgprot_val(mask_clr) && !force_split)
		return 0;

	/* Ensure we are PAGE_SIZE aligned */
	if (in_flag & CPA_ARRAY) {
		int i;

		for (i = 0; i < numpages; i++) {
			if (addr[i] & ~PAGE_MASK) {
				addr[i] &= PAGE_MASK;
				WARN_ON_ONCE(1);
			}
		}
	} else if (!(in_flag & CPA_PAGES_ARRAY)) {
		/*
		 * in_flag of CPA_PAGES_ARRAY implies it is aligned.
		 * No need to check in that case
		 */
		if (*addr & ~PAGE_MASK) {
			*addr &= PAGE_MASK;
			/*
			 * People should not be passing in unaligned addresses:
			 */
			WARN_ON_ONCE(1);
		}
	}

	/* Must avoid aliasing mappings in the highmem code */
	kmap_flush_unused();

	vm_unmap_aliases();

	cpa.vaddr = addr;
	cpa.pages = pages;
	cpa.numpages = numpages;
	cpa.mask_set = mask_set;
	cpa.mask_clr = mask_clr;
	cpa.flags = in_flag;
	cpa.curpage = 0;
	cpa.force_split = force_split;

	err = __change_page_attr_set_clr(&cpa, 1);

	/*
	 * Check whether we really changed something:
	 */
	if (!(cpa.flags & CPA_FLUSHTLB))
		goto out;

	arch_cpa_flush(&cpa, err);

out:
	return err;
}
