// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory Encryption Support Common Code
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 */

#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/swiotlb.h>
#include <linux/cc_platform.h>
#include <linux/mem_encrypt.h>
#include <linux/pgalloc.h>
#include <linux/percpu.h>
#include <linux/virtio_anchor.h>
#include <linux/iommu-dma.h>

#include <asm/sections.h>
#include <asm/set_memory.h>
#include <asm/sev.h>
#include <asm/tlbflush.h>
#include <asm/x86_init.h>

#include "mm_internal.h"

extern char __percpu __start_percpu_decrypted[], __end_percpu_decrypted[];

static pte_t * __init early_lookup_pte(unsigned long addr)
{
	unsigned long pfn, step;
	unsigned int level, i;
	pgprot_t prot;
	pte_t *pte, *table;

	for (;;) {
		pte = lookup_address(addr, &level);
		if (!pte || !pte_present(*pte))
			return NULL;
		if (level == PG_LEVEL_4K)
			return pte;

		if (level == PG_LEVEL_2M) {
			pfn = pmd_pfn(*(pmd_t *)pte);
			prot = pgprot_large_2_4k(pmd_pgprot(*(pmd_t *)pte));
			step = 1;
		} else if (level == PG_LEVEL_1G) {
			pfn = pud_pfn(*(pud_t *)pte);
			prot = pud_pgprot(*(pud_t *)pte);
			step = PMD_SIZE >> PAGE_SHIFT;
		} else {
			return NULL;
		}

		table = alloc_low_page();
		if (!table)
			return NULL;
		for (i = 0; i < PTRS_PER_PTE; i++, pfn += step)
			set_pte(&table[i], pfn_pte(pfn, prot));

		if (level == PG_LEVEL_2M)
			pmd_populate_kernel(&init_mm, (pmd_t *)pte, table);
		else
			pud_populate(&init_mm, (pud_t *)pte, (pmd_t *)table);

		if (addr - PAGE_OFFSET < get_max_mapped()) {
			update_page_count(level, -1);
			update_page_count(level - 1, PTRS_PER_PTE);
		}

		/* Flush the large translation before changing any attributes. */
		__flush_tlb_all();
	}
}

/* The caller has split both mappings before starting the page transition. */
void __init early_set_page_decrypted(unsigned long addr, unsigned long alias)
{
	unsigned int level;
	pte_t *pte;

	pte = lookup_address(addr, &level);
	set_pte(pte, __pte(cc_mkdec(pte_val(*pte))));
	if (alias) {
		pte = lookup_address(alias, &level);
		set_pte(pte, __pte(cc_mkdec(pte_val(*pte))));
	}
	__flush_tlb_all();
}

/* Boot CPU only; size is in bytes and the contents are preserved. */
int __init early_set_memory_decrypted(unsigned long vaddr, unsigned long size)
{
	unsigned long end, addr, alias, pa;
	pte_t *pte;
	int ret;

	if (!size || !cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return 0;
	if (!x86_init.paging.early_decrypt_page)
		return -EOPNOTSUPP;
	if (size > ULONG_MAX - vaddr)
		return -EINVAL;
	end = PAGE_ALIGN(vaddr + size);
	if (end < vaddr)
		return -EINVAL;

	for (vaddr &= PAGE_MASK; vaddr < end; vaddr += PAGE_SIZE) {
		if (vaddr - PAGE_OFFSET >= get_max_mapped() &&
		    (vaddr < (unsigned long)_text || vaddr >= _brk_end))
			return -EINVAL;

		pa = __pa(vaddr);
		addr = (unsigned long)__va(pa);
		pte = early_lookup_pte(addr);
		if (!pte || pte_pfn(*pte) != PHYS_PFN(pa))
			return -EFAULT;

		alias = 0;
		if (pa >= __pa_symbol(_text) &&
		    pa <= __pa_symbol(roundup(_brk_end, PMD_SIZE) - 1)) {
			alias = (unsigned long)_text + pa - __pa_symbol(_text);
			if (!early_lookup_pte(alias))
				return -EFAULT;
		}

		if (pte_decrypted(*pte)) {
			early_set_page_decrypted(addr, alias);
			continue;
		}
		ret = x86_init.paging.early_decrypt_page(addr, alias);
		if (ret)
			return ret;
	}

	return 0;
}

void __init mem_encrypt_init_percpu(void)
{
	unsigned long size = __end_percpu_decrypted - __start_percpu_decrypted;
	int cpu, ret;

	if (!cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT) ||
	    x86_init.paging.skip_percpu_decryption)
		return;

	for_each_possible_cpu(cpu) {
		unsigned long addr = (unsigned long)
			per_cpu_ptr(__start_percpu_decrypted, cpu);

		ret = early_set_memory_decrypted(addr, size);
		if (ret)
			panic("Cannot share CPU %d per-CPU data (err=%d)", cpu, ret);
	}
}

/* Override for DMA direct allocation check - ARCH_HAS_FORCE_DMA_UNENCRYPTED */
bool force_dma_unencrypted(struct device *dev)
{
	/*
	 * For SEV, all DMA must be to unencrypted addresses.
	 */
	if (cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT))
		return true;

	/*
	 * For SME, all DMA must be to unencrypted addresses if the
	 * device does not support DMA to addresses that include the
	 * encryption mask.
	 */
	if (cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT) && !use_dma_iommu(dev)) {
		u64 dma_enc_mask = DMA_BIT_MASK(__ffs64(sme_me_mask));
		u64 dma_dev_mask = min_not_zero(dev->coherent_dma_mask,
						dev->bus_dma_limit);

		if (dma_dev_mask <= dma_enc_mask)
			return true;
	}

	return false;
}

static void print_mem_encrypt_feature_info(void)
{
	pr_info("Memory Encryption Features active: ");

	switch (cc_vendor) {
	case CC_VENDOR_INTEL:
		pr_cont("Intel TDX\n");
		break;
	case CC_VENDOR_AMD:
		pr_cont("AMD");

		/* Secure Memory Encryption */
		if (cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT)) {
		/*
		 * SME is mutually exclusive with any of the SEV
		 * features below.
		*/
			pr_cont(" SME\n");
			return;
		}

		/* Secure Encrypted Virtualization */
		if (cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT))
			pr_cont(" SEV");

		/* Encrypted Register State */
		if (cc_platform_has(CC_ATTR_GUEST_STATE_ENCRYPT))
			pr_cont(" SEV-ES");

		/* Secure Nested Paging */
		if (cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
			pr_cont(" SEV-SNP");

		pr_cont("\n");

		sev_show_status();

		break;
	default:
		pr_cont("Unknown\n");
	}
}

/* Architecture __weak replacement functions */
void __init mem_encrypt_init(void)
{
	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return;

	/* Call into SWIOTLB to update the SWIOTLB DMA buffers */
	swiotlb_update_mem_attributes();

	snp_secure_tsc_prepare();

	print_mem_encrypt_feature_info();
}

void __init mem_encrypt_setup_arch(void)
{
	phys_addr_t total_mem = memblock_phys_mem_size();
	unsigned long size;

	/*
	 * Do RMP table fixups after the e820 tables have been setup by
	 * e820__memory_setup().
	 */
	if (cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		snp_fixup_e820_tables();

	if (!cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT))
		return;

	/*
	 * For SEV and TDX, all DMA has to occur via shared/unencrypted pages.
	 * Kernel uses SWIOTLB to make this happen without changing device
	 * drivers. However, depending on the workload being run, the
	 * default 64MB of SWIOTLB may not be enough and SWIOTLB may
	 * run out of buffers for DMA, resulting in I/O errors and/or
	 * performance degradation especially with high I/O workloads.
	 *
	 * Adjust the default size of SWIOTLB using a percentage of guest
	 * memory for SWIOTLB buffers. Also, as the SWIOTLB bounce buffer
	 * memory is allocated from low memory, ensure that the adjusted size
	 * is within the limits of low available memory.
	 *
	 * The percentage of guest memory used here for SWIOTLB buffers
	 * is more of an approximation of the static adjustment which
	 * 64MB for <1G, and ~128M to 256M for 1G-to-4G, i.e., the 6%
	 */
	size = total_mem * 6 / 100;
	size = clamp_val(size, IO_TLB_DEFAULT_SIZE, SZ_1G);
	swiotlb_adjust_size(size);

	/* Set restricted memory access for virtio. */
	virtio_set_mem_acc_cb(virtio_require_restricted_mem_acc);
}
