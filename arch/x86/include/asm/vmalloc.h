#ifndef _ASM_X86_VMALLOC_H
#define _ASM_X86_VMALLOC_H

#include <asm/cpufeature.h>
#include <asm/page.h>
#include <asm/pgtable_areas.h>

/*
 * The x86 ENTER instruction can be used as a one-instruction stack pivot:
 * ENTER imm16, imm8 lowers RSP by imm16 + 8 * (L + 1), L = imm8 & 0x1f.
 * imm16 is an unsigned 16-bit operand (ENTER never raises RSP) and L is in
 * [0, 31], so a single ENTER can lower RSP by at most
 * 0xffff + 8 * 0x20 = 0x100ff bytes. With CONFIG_VMAP_STACK the kernel
 * stack lives in the vmalloc area, where an unprivileged user can spray
 * adjacent allocations; a single-page guard is too small to contain such a
 * pivot. Use 0x11 guard pages (0x11000 bytes), the smallest whole-page
 * span exceeding 0x100ff, so the pivot faults in the guard instead of
 * landing in attacker-controlled memory.
 *
 * Restrict this to 64-bit: VMAP_STACK is selected only on x86_64, so 32-bit
 * kernel stacks are not in the vmalloc area and the technique does not apply.
 * 32-bit also has a far smaller vmalloc window, where a 16-page-per-area
 * widening would needlessly pressure the address space.
 */
#ifdef CONFIG_X86_64
#define VMAP_GUARD_PAGES	0x11
#endif

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP

#ifdef CONFIG_X86_64
#define arch_vmap_pud_supported arch_vmap_pud_supported
static inline bool arch_vmap_pud_supported(pgprot_t prot)
{
	return boot_cpu_has(X86_FEATURE_GBPAGES);
}
#endif

#define arch_vmap_pmd_supported arch_vmap_pmd_supported
static inline bool arch_vmap_pmd_supported(pgprot_t prot)
{
	return boot_cpu_has(X86_FEATURE_PSE);
}

#endif

#endif /* _ASM_X86_VMALLOC_H */
