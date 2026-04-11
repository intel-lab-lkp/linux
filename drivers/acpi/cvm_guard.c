// SPDX-License-Identifier: GPL-2.0
/*
 * CVM Guard - Block AML access to confidential VM private memory
 *
 * Copyright (C) 2026 Privasys
 *
 * On TDX and SEV-SNP guests the host VMM controls ACPI tables, so
 * AML bytecode executing SystemMemory reads and writes can target
 * arbitrary guest physical addresses.  This file provides a guard
 * function called from the ACPICA SystemMemory space handler that
 * checks whether the target virtual address maps to a page marked
 * as encrypted (private) in the page tables, and denies the access
 * if so.
 *
 * Reference: "BadAML: Exploiting AML in Confidential Virtual Machines"
 *            Takekoshi et al., ACM CCS 2025
 */

#include <linux/cc_platform.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <asm/coco.h>

/* Prototype to satisfy -Wmissing-prototypes; declared here rather than in
 * internal.h because this file does not need the full ACPI driver headers.
 */
bool acpi_cvm_guard_deny_access(unsigned long virt_addr);

/*
 * Walk the four-level kernel page tables for @addr and return the raw
 * PTE/PMD/PUD value.  Returns 0 if the walk fails at any level.
 * Handles 1 GB (PUD) and 2 MB (PMD) large pages.
 */
static unsigned long cvm_guard_pte_val(unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return 0;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return 0;
	if (pud_leaf(*pud))
		return pud_val(*pud);

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;
	if (pmd_leaf(*pmd))
		return pmd_val(*pmd);

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pte_val(*pte);
}

/*
 * Check whether @addr maps to a private (encrypted) page.
 *
 * cc_mkenc() applies the platform-specific encryption mask:
 *   AMD SEV/SEV-SNP: sets the C-bit
 *   Intel TDX:       clears the shared bit
 *
 * If the PTE already matches its encrypted form, the page is private
 * and must not be accessible to AML.  If the walk fails (returns 0)
 * we deny access - fail-closed is the safe default.
 */
static bool cvm_guard_page_is_private(unsigned long addr)
{
	unsigned long val;

	val = cvm_guard_pte_val(addr);
	if (!val) {
		pr_warn_ratelimited("CVM guard: page table walk failed for %lx\n",
				    addr);
		return true;
	}

	return val == cc_mkenc(val);
}

/**
 * acpi_cvm_guard_deny_access - block AML access to CVM private pages
 * @virt_addr: kernel virtual address resolved by the SystemMemory handler
 *
 * Called from acpi_ex_system_memory_space_handler() after the virtual
 * address has been computed but before any read or write.
 *
 * On non-CVM systems (CC_ATTR_MEM_ENCRYPT not set) this returns false.
 *
 * Return: true if the access must be denied, false if allowed.
 */
bool acpi_cvm_guard_deny_access(unsigned long virt_addr)
{
	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return false;

	pr_info_once("CVM guard: active, AML access to private pages will be denied\n");

	virt_addr &= PAGE_MASK;

	if (cvm_guard_page_is_private(virt_addr)) {
		pr_warn_ratelimited("CVM guard: denied AML access to private page at %lx\n",
				    virt_addr);
		return true;
	}

	return false;
}
