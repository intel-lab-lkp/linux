// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/arch/arm/mm/mmap.c
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/shm.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/io.h>
#include <linux/personality.h>
#include <linux/random.h>
#include <asm/cachetype.h>

#define COLOUR_ALIGN(addr,pgoff)		\
	((((addr)+SHMLBA-1)&~(SHMLBA-1)) +	\
	 (((pgoff)<<PAGE_SHIFT) & (SHMLBA-1)))

unsigned long arch_mmap_hint(struct file *filp, unsigned long addr,
			     unsigned long len, unsigned long pgoff,
			     unsigned long flags)
{
	bool aliasing = cache_is_vipt_aliasing();
	bool do_align;

	/* requested length too big for entire address space */
	if (len > TASK_SIZE)
		return -ENOMEM;

	/*
	 * We enforce the MAP_FIXED case.
	 */
	if (flags & MAP_FIXED) {
		if (aliasing && flags & MAP_SHARED &&
		    (addr - (pgoff << PAGE_SHIFT)) & (SHMLBA - 1))
			return -EINVAL;
		return addr;
	}

	/*
	 * We only need to do colour alignment if either the I or D
	 * caches alias.
	 */
	do_align = aliasing && (filp || (flags & MAP_SHARED));

	if (do_align)
		addr = COLOUR_ALIGN(addr, pgoff);
	else
		addr = PAGE_ALIGN(addr);

	return generic_mmap_hint(filp, addr, len, pgoff, flags);
}

/*
 * We need to ensure that shared mappings are correctly aligned to
 * avoid aliasing issues with VIPT caches.  We need to ensure that
 * a specific page of an object is always mapped at a multiple of
 * SHMLBA bytes.
 *
 * We unconditionally provide this function for all cases, however
 * in the VIVT case, we optimise out the alignment rules.
 */
unsigned long
arch_get_unmapped_area(struct file *filp, unsigned long addr,
		unsigned long len, unsigned long pgoff,
		unsigned long flags, vm_flags_t vm_flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_unmapped_area_info info = {};
	bool aliasing = cache_is_vipt_aliasing();
	bool do_align;

	addr = arch_mmap_hint(filp, addr, len, pgoff, flags);
	if (addr)
		return addr;

	do_align = aliasing && (filp || (flags & MAP_SHARED));

	info.length = len;
	info.low_limit = mm->mmap_base;
	info.high_limit = TASK_SIZE;
	info.align_mask = do_align ? (PAGE_MASK & (SHMLBA - 1)) : 0;
	info.align_offset = pgoff << PAGE_SHIFT;
	return vm_unmapped_area(&info);
}

unsigned long
arch_get_unmapped_area_topdown(struct file *filp, const unsigned long addr0,
		        const unsigned long len, const unsigned long pgoff,
		        const unsigned long flags, vm_flags_t vm_flags)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr = addr0;
	struct vm_unmapped_area_info info = {};
	bool aliasing = cache_is_vipt_aliasing();
	bool do_align;

	addr = arch_mmap_hint(filp, addr, len, pgoff, flags);
	if (addr)
		return addr;

	do_align = aliasing && (filp || (flags & MAP_SHARED));

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = FIRST_USER_ADDRESS;
	info.high_limit = mm->mmap_base;
	info.align_mask = do_align ? (PAGE_MASK & (SHMLBA - 1)) : 0;
	info.align_offset = pgoff << PAGE_SHIFT;
	addr = vm_unmapped_area(&info);

	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	if (addr & ~PAGE_MASK) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = mm->mmap_base;
		info.high_limit = TASK_SIZE;
		addr = vm_unmapped_area(&info);
	}

	return addr;
}

/*
 * You really shouldn't be using read() or write() on /dev/mem.  This
 * might go away in the future.
 */
int valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	if (addr < PHYS_OFFSET)
		return 0;
	if (addr + size > __pa(high_memory - 1) + 1)
		return 0;

	return 1;
}

/*
 * Do not allow /dev/mem mappings beyond the supported physical range.
 */
int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	return (pfn + (size >> PAGE_SHIFT)) <= (1 + (PHYS_MASK >> PAGE_SHIFT));
}
