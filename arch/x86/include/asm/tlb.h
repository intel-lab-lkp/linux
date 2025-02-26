/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_TLB_H
#define _ASM_X86_TLB_H

#define tlb_flush tlb_flush
static inline void tlb_flush(struct mmu_gather *tlb);

#include <asm-generic/tlb.h>
#include <linux/kernel.h>
#include <vdso/bits.h>
#include <vdso/page.h>

static inline void tlb_flush(struct mmu_gather *tlb)
{
	unsigned long start = 0UL, end = TLB_FLUSH_ALL;
	unsigned int stride_shift = tlb_get_unmap_shift(tlb);

	if (!tlb->fullmm && !tlb->need_flush_all) {
		start = tlb->start;
		end = tlb->end;
	}

	flush_tlb_mm_range(tlb->mm, start, end, stride_shift, tlb->freed_tables);
}

static inline void invlpg(unsigned long addr)
{
	asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}


/*
 * INVLPGB does broadcast TLB invalidation across all the CPUs in the system.
 *
 * The INVLPGB instruction is weakly ordered, and a batch of invalidations can
 * be done in a parallel fashion.
 *
 * The instruction takes the number of extra pages to invalidate, beyond
 * the first page, while __invlpgb gets the more human readable number of
 * pages to invalidate.
 *
 * TLBSYNC is used to ensure that pending INVLPGB invalidations initiated from
 * this CPU have completed.
 */
static inline void __invlpgb(unsigned long asid, unsigned long pcid,
			     unsigned long addr, u16 nr_pages,
			     bool pmd_stride, u8 flags)
{
	u32 edx = (pcid << 16) | asid;
	u32 ecx = (pmd_stride << 31) | (nr_pages - 1);
	u64 rax = addr | flags;

	/* The low bits in rax are for flags. Verify addr is clean. */
	VM_WARN_ON_ONCE(addr & ~PAGE_MASK);

	/* INVLPGB; supported in binutils >= 2.36. */
	asm volatile(".byte 0x0f, 0x01, 0xfe" : : "a" (rax), "c" (ecx), "d" (edx));
}

static inline void __tlbsync(void)
{
	/*
	 * tlbsync waits for invlpgb instructions originating on the
	 * same CPU to have completed. Print a warning if we could have
	 * migrated, and might not be waiting on all the invlpgbs issued
	 * during this TLB invalidation sequence.
	 */
	cant_migrate();

	/* TLBSYNC: supported in binutils >= 0.36. */
	asm volatile(".byte 0x0f, 0x01, 0xff" ::: "memory");
}

/*
 * INVLPGB can be targeted by virtual address, PCID, ASID, or any combination
 * of the three. For example:
 * - INVLPGB_VA | INVLPGB_INCLUDE_GLOBAL: invalidate all TLB entries at the address
 * - INVLPGB_PCID:			  invalidate all TLB entries matching the PCID
 *
 * The first can be used to invalidate (kernel) mappings at a particular
 * address across all processes.
 *
 * The latter invalidates all TLB entries matching a PCID.
 */
#define INVLPGB_VA			BIT(0)
#define INVLPGB_PCID			BIT(1)
#define INVLPGB_ASID			BIT(2)
#define INVLPGB_INCLUDE_GLOBAL		BIT(3)
#define INVLPGB_FINAL_ONLY		BIT(4)
#define INVLPGB_INCLUDE_NESTED		BIT(5)

static inline void invlpgb_flush_user_nr_nosync(unsigned long pcid,
						unsigned long addr,
						u16 nr,
						bool pmd_stride)
{
	__invlpgb(0, pcid, addr, nr, pmd_stride, INVLPGB_PCID | INVLPGB_VA);
}

/* Flush all mappings for a given PCID, not including globals. */
static inline void invlpgb_flush_single_pcid_nosync(unsigned long pcid)
{
	__invlpgb(0, pcid, 0, 1, 0, INVLPGB_PCID);
}

/* Flush all mappings, including globals, for all PCIDs. */
static inline void invlpgb_flush_all(void)
{
	__invlpgb(0, 0, 0, 1, 0, INVLPGB_INCLUDE_GLOBAL);
	__tlbsync();
}

/* Flush addr, including globals, for all PCIDs. */
static inline void invlpgb_flush_addr_nosync(unsigned long addr, u16 nr)
{
	__invlpgb(0, 0, addr, nr, 0, INVLPGB_INCLUDE_GLOBAL);
}

/* Flush all mappings for all PCIDs except globals. */
static inline void invlpgb_flush_all_nonglobals(void)
{
	__invlpgb(0, 0, 0, 1, 0, 0);
	__tlbsync();
}

#endif /* _ASM_X86_TLB_H */
