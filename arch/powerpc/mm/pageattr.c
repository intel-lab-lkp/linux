// SPDX-License-Identifier: GPL-2.0

/*
 * MMU-generic set_memory implementation for powerpc
 *
 * Copyright 2019-2021, IBM Corporation.
 */

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>

#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#include <mm/mmu_decl.h>

static pte_basic_t pte_update_delta(pte_t *ptep, unsigned long addr,
				    unsigned long old, unsigned long new)
{
	return pte_update(&init_mm, addr, ptep, old & ~new, new & ~old, 0);
}

/*
 * Updates the attributes of a page atomically.
 *
 * This sequence is safe against concurrent updates, and also allows updating the
 * attributes of a page currently being executed or accessed.
 */
static int change_page_attr(pte_t *ptep, unsigned long addr, void *data)
{
	long action = (long)data;

	addr &= PAGE_MASK;
	/* modify the PTE bits as desired */
	switch (action) {
	case SET_MEMORY_RO:
		/* Don't clear DIRTY bit */
		pte_update_delta(ptep, addr, _PAGE_KERNEL_RW & ~_PAGE_DIRTY, _PAGE_KERNEL_RO);
		break;
	case SET_MEMORY_ROX:
		/* Don't clear DIRTY bit */
		pte_update_delta(ptep, addr, _PAGE_KERNEL_RW & ~_PAGE_DIRTY, _PAGE_KERNEL_ROX);
		break;
	case SET_MEMORY_RW:
		pte_update_delta(ptep, addr, _PAGE_KERNEL_RO, _PAGE_KERNEL_RW);
		break;
	case SET_MEMORY_NX:
		pte_update_delta(ptep, addr, _PAGE_KERNEL_ROX, _PAGE_KERNEL_RO);
		break;
	case SET_MEMORY_X:
		pte_update_delta(ptep, addr, _PAGE_KERNEL_RO, _PAGE_KERNEL_ROX);
		break;
	case SET_MEMORY_NP:
		pte_update(&init_mm, addr, ptep, _PAGE_PRESENT, 0, 0);
		break;
	case SET_MEMORY_P:
		pte_update(&init_mm, addr, ptep, 0, _PAGE_PRESENT, 0);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	/* See ptesync comment in radix__set_pte_at() */
	if (radix_enabled())
		asm volatile("ptesync": : :"memory");

	flush_tlb_kernel_range(addr, addr + PAGE_SIZE);

	return 0;
}

int change_memory_attr(unsigned long addr, int numpages, long action)
{
	unsigned long start = ALIGN_DOWN(addr, PAGE_SIZE);
	unsigned long size = numpages * PAGE_SIZE;

	if (!numpages)
		return 0;

	if (WARN_ON_ONCE(is_vmalloc_or_module_addr((void *)addr) &&
			 is_vm_area_hugepages((void *)addr)))
		return -EINVAL;

#ifdef CONFIG_PPC_BOOK3S_64
	/*
	 * On hash, the linear mapping is not in the Linux page table so
	 * apply_to_existing_page_range() will have no effect. If in the future
	 * the set_memory_* functions are used on the linear map this will need
	 * to be updated.
	 */
	if (!radix_enabled()) {
		int region = get_region_id(addr);

		if (WARN_ON_ONCE(region != VMALLOC_REGION_ID && region != IO_REGION_ID))
			return -EINVAL;
	}
#endif

	return apply_to_existing_page_range(&init_mm, start, size,
					    change_page_attr, (void *)action);
}

#ifdef CONFIG_ARCH_HAS_SET_DIRECT_MAP
/*
 * Removing a single page from the linear mapping requires that the linear
 * mapping is built out of PAGE_SIZE mappings in the first place. Radix only
 * does that when explicitly asked to and there is no support for splitting
 * a live 2M/1G mapping yet.
 * TODO: Hash is also not supported yet.
 */
bool can_set_direct_map(void)
{
	if (!radix_enabled())
		return false;

	return linear_map_uses_base_pagesize();
}

/*
 * Note that the set_memory_p()/set_memory_np() helpers below already flush the
 * TLB for the range they touch, so the _noflush() naming is not honoured
 * literally here.
 * Since as of now this is not in the performance path, so it should be ok.
 * Later, once the support for breaking the large mappings (PUD/PMD) into PTE is
 * added, then this can be improved since it will anyway require a custom
 * implementation for walking the page table and breaking the mapping.
 */
int set_direct_map_invalid_noflush(struct page *page, unsigned int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	if (!can_set_direct_map())
		return 0;

	return set_memory_np(addr, numpages);
}

int set_direct_map_default_noflush(struct page *page, unsigned int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	if (!can_set_direct_map())
		return 0;

	return set_memory_p(addr, numpages);
}

/*
 * Walk the kernel page table looking for the entry that translates @addr.
 * Currently secretmemfd is the only usecase for this.
 */
static bool radix_page_present(unsigned long addr)
{
	pgd_t *pgdp;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep;

	pgdp = pgd_offset_k(addr);

	p4dp = p4d_offset(pgdp, addr);
	p4d = READ_ONCE(*p4dp);
	if (p4d_none(p4d))
		return false;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_leaf(pud))
		return pud_present(pud);

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_leaf(pmd))
		return pmd_present(pmd);

	ptep = pte_offset_kernel(pmdp, addr);
	return pte_hw_valid(ptep_get(ptep));
}

bool kernel_page_present(struct page *page)
{
	/*
	 * Nothing ever removes entries from the linear mapping in this
	 * configuration, so everything is present.
	 */
	if (!can_set_direct_map())
		return true;

	return radix_page_present((unsigned long)page_address(page));
}
#endif /* CONFIG_ARCH_HAS_SET_DIRECT_MAP */


#if defined(CONFIG_DEBUG_PAGEALLOC) || defined(CONFIG_KFENCE)
#ifdef CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	int err;
	unsigned long addr = (unsigned long)page_address(page);

	if (PageHighMem(page))
		return;

	if (IS_ENABLED(CONFIG_PPC_BOOK3S_64) && !radix_enabled())
		err = hash__kernel_map_pages(page, numpages, enable);
	else if (enable)
		err = set_memory_p(addr, numpages);
	else
		err = set_memory_np(addr, numpages);

	if (err)
		panic("%s: changing memory protections failed\n", __func__);
}
#endif
#endif
