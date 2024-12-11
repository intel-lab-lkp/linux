/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2011 Wind River Systems,
 *   written by Ralf Baechle <ralf@linux-mips.org>
 */
#include <linux/compiler.h>
#include <linux/elf-randomize.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/export.h>
#include <linux/personality.h>
#include <linux/random.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>

unsigned long shm_align_mask = PAGE_SIZE - 1;	/* Sane caches */
EXPORT_SYMBOL(shm_align_mask);

#define COLOUR_ALIGN(addr, pgoff)				\
	((((addr) + shm_align_mask) & ~shm_align_mask) +	\
	 (((pgoff) << PAGE_SHIFT) & shm_align_mask))

enum mmap_allocation_direction {UP, DOWN};

unsigned long arch_mmap_hint(struct file *filp, unsigned long addr,
			     unsigned long len, unsigned long pgoff,
			     unsigned long flags)
{
	bool do_color_align;

	if (unlikely(len > TASK_SIZE))
		return -ENOMEM;

	if (flags & MAP_FIXED) {
		/* Even MAP_FIXED mappings must reside within TASK_SIZE */
		if (TASK_SIZE - len < addr)
			return -EINVAL;

		/*
		 * We do not accept a shared mapping if it would violate
		 * cache aliasing constraints.
		 */
		if ((flags & MAP_SHARED) &&
		    ((addr - (pgoff << PAGE_SHIFT)) & shm_align_mask))
			return -EINVAL;
		return addr;
	}

	do_color_align = filp || (flags & MAP_SHARED);

	if (do_color_align)
		addr = COLOUR_ALIGN(addr, pgoff);
	else
		addr = PAGE_ALIGN(addr);

	return generic_mmap_hint(filp, addr, len, pgoff, flags);
}


static unsigned long arch_get_unmapped_area_common(struct file *filp,
	unsigned long addr0, unsigned long len, unsigned long pgoff,
	unsigned long flags, enum mmap_allocation_direction dir)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr = addr0;
	struct vm_unmapped_area_info info = {};
	bool do_color_align;

	/* requesting a specific address */
	addr = arch_mmap_hint(filp, addr, len, pgoff, flags);
	if (addr)
		return addr;

	do_color_align = filp || (flags & MAP_SHARED);

	info.length = len;
	info.align_mask = do_color_align ? (PAGE_MASK & shm_align_mask) : 0;
	info.align_offset = pgoff << PAGE_SHIFT;

	if (dir == DOWN) {
		info.flags = VM_UNMAPPED_AREA_TOPDOWN;
		info.low_limit = PAGE_SIZE;
		info.high_limit = mm->mmap_base;
		addr = vm_unmapped_area(&info);

		if (!(addr & ~PAGE_MASK))
			return addr;

		/*
		 * A failed mmap() very likely causes application failure,
		 * so fall back to the bottom-up function here. This scenario
		 * can happen with large stack limits and large mmap()
		 * allocations.
		 */
	}

	info.low_limit = mm->mmap_base;
	info.high_limit = TASK_SIZE;
	return vm_unmapped_area(&info);
}

unsigned long arch_get_unmapped_area(struct file *filp, unsigned long addr0,
	unsigned long len, unsigned long pgoff, unsigned long flags,
	vm_flags_t vm_flags)
{
	return arch_get_unmapped_area_common(filp,
			addr0, len, pgoff, flags, UP);
}

/*
 * There is no need to export this but sched.h declares the function as
 * extern so making it static here results in an error.
 */
unsigned long arch_get_unmapped_area_topdown(struct file *filp,
	unsigned long addr0, unsigned long len, unsigned long pgoff,
	unsigned long flags, vm_flags_t vm_flags)
{
	return arch_get_unmapped_area_common(filp,
			addr0, len, pgoff, flags, DOWN);
}

bool __virt_addr_valid(const volatile void *kaddr)
{
	unsigned long vaddr = (unsigned long)kaddr;

	if ((vaddr < PAGE_OFFSET) || (vaddr >= MAP_BASE))
		return false;

	return pfn_valid(PFN_DOWN(virt_to_phys(kaddr)));
}
EXPORT_SYMBOL_GPL(__virt_addr_valid);
