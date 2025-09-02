/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PAGE_64_H
#define _ASM_X86_PAGE_64_H

#include <asm/page_64_types.h>

#ifndef __ASSEMBLER__
#include <asm/cpufeatures.h>
#include <asm/alternative.h>

#include <linux/kmsan-checks.h>

/* duplicated to the one in bootmem.h */
extern unsigned long max_pfn;
extern unsigned long phys_base;

extern unsigned long page_offset_base;
extern unsigned long vmalloc_base;
extern unsigned long vmemmap_base;
extern unsigned long direct_map_physmem_end;

static __always_inline unsigned long __phys_addr_nodebug(unsigned long x)
{
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	x = y + ((x > y) ? phys_base : (__START_KERNEL_map - PAGE_OFFSET));

	return x;
}

#ifdef CONFIG_DEBUG_VIRTUAL
extern unsigned long __phys_addr(unsigned long);
extern unsigned long __phys_addr_symbol(unsigned long);
#else
#define __phys_addr(x)		__phys_addr_nodebug(x)
#define __phys_addr_symbol(x) \
	((unsigned long)(x) - __START_KERNEL_map + phys_base)
#endif

#define __phys_reloc_hide(x)	(x)

void memzero_page_aligned_unrolled(void *addr, u64 len);

/**
 * clear_page() - clear a page using a kernel virtual address.
 * @page: address of kernel page
 *
 * Switch between three implementations of page clearing based on CPU
 * capabilities:
 *
 *  - memzero_page_aligned_unrolled(): the oldest, slowest and universally
 *    supported method. Zeroes via 8-byte MOV instructions unrolled 8x
 *    to write a 64-byte cacheline in each loop iteration..
 *
 *  - "rep stosq": really old CPUs had crummy REP implementations.
 *    Vendor CPU setup code sets 'REP_GOOD' on CPUs where REP can be
 *    trusted. The instruction writes 8-byte per REP iteration but
 *    CPUs can internally batch these together and do larger writes.
 *
 *  - "rep stosb": CPUs that enumerate 'ERMS' have an improved STOS
 *    implementation that is less picky about alignment and where
 *    STOSB (1-byte at a time) is actually faster than STOSQ (8-bytes
 *    at a time.)
 *
 * Does absolutely no exception handling.
 */
static inline void clear_page(void *page)
{
	u64 len = PAGE_SIZE;
	/*
	 * Clean up KMSAN metadata for the page being cleared. The assembly call
	 * below clobbers @page, so we perform unpoisoning before it.
	 */
	kmsan_unpoison_memory(page, len);
	asm volatile(ALTERNATIVE_2("call memzero_page_aligned_unrolled",
				   "shrq $3, %%rcx; rep stosq", X86_FEATURE_REP_GOOD,
				   "rep stosb", X86_FEATURE_ERMS)
			: "+c" (len), "+D" (page), ASM_CALL_CONSTRAINT
			: "a" (0)
			: "cc", "memory");
}

void copy_page(void *to, void *from);
KCFI_REFERENCE(copy_page);

/*
 * User space process size.  This is the first address outside the user range.
 * There are a few constraints that determine this:
 *
 * On Intel CPUs, if a SYSCALL instruction is at the highest canonical
 * address, then that syscall will enter the kernel with a
 * non-canonical return address, and SYSRET will explode dangerously.
 * We avoid this particular problem by preventing anything
 * from being mapped at the maximum canonical address.
 *
 * On AMD CPUs in the Ryzen family, there's a nasty bug in which the
 * CPUs malfunction if they execute code from the highest canonical page.
 * They'll speculate right off the end of the canonical space, and
 * bad things happen.  This is worked around in the same way as the
 * Intel problem.
 *
 * With page table isolation enabled, we map the LDT in ... [stay tuned]
 */
static __always_inline unsigned long task_size_max(void)
{
	unsigned long ret;

	alternative_io("movq %[small],%0","movq %[large],%0",
			X86_FEATURE_LA57,
			"=r" (ret),
			[small] "i" ((1ul << 47)-PAGE_SIZE),
			[large] "i" ((1ul << 56)-PAGE_SIZE));

	return ret;
}

#endif	/* !__ASSEMBLER__ */

#ifdef CONFIG_X86_VSYSCALL_EMULATION
# define __HAVE_ARCH_GATE_AREA 1
#endif

#endif /* _ASM_X86_PAGE_64_H */
