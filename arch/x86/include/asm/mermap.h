/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MERMAP_H
#define _ASM_X86_MERMAP_H

#include <asm/tlbflush.h>

static inline void arch_mermap_flush_tlb(void)
{
	/*
	 * No shootdown allowed, IRQs may be off. Luckily other CPUs are not
	 * allowed to access our region so the stale mappings are harmless, as
	 * long as they still point to data belonging to this process.
	 */
	__flush_tlb_all();
}

static inline bool arch_mermap_pgprot_allowed(pgprot_t prot)
{
	/* Mermap is mm-local so global mappings would be a bug. */
	return !(pgprot_val(prot) & _PAGE_GLOBAL);
}

#endif /* _ASM_X86_MERMAP_H */
