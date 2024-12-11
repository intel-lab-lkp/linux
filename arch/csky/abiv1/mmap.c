// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Hangzhou C-SKY Microsystems co.,ltd.

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/shm.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/io.h>

#define COLOUR_ALIGN(addr,pgoff)		\
	((((addr)+SHMLBA-1)&~(SHMLBA-1)) +	\
	 (((pgoff)<<PAGE_SHIFT) & (SHMLBA-1)))

unsigned long arch_mmap_hint(struct file *filp, unsigned long addr,
			     unsigned long len, unsigned long pgoff,
			     unsigned long flags)
{
	bool do_align;

	if (len > TASK_SIZE)
		return -ENOMEM;

	/*
	 * We only need to do colour alignment if either the I or D
	 * caches alias.
	 */
	do_align = filp || (flags & MAP_SHARED);

	/*
	 * We enforce the MAP_FIXED case.
	 */
	if (flags & MAP_FIXED) {
		if (flags & MAP_SHARED &&
		    (addr - (pgoff << PAGE_SHIFT)) & (SHMLBA - 1))
			return -EINVAL;
		return addr;
	}

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
 * We unconditionally provide this function for all cases.
 */
unsigned long
arch_get_unmapped_area(struct file *filp, unsigned long addr,
		unsigned long len, unsigned long pgoff,
		unsigned long flags, vm_flags_t vm_flags)
{
	struct mm_struct *mm = current->mm;
	bool do_align;
	struct vm_unmapped_area_info info = {
		.length = len,
		.low_limit = mm->mmap_base,
		.high_limit = TASK_SIZE,
		.align_offset = pgoff << PAGE_SHIFT
	};

	addr = arch_mmap_hint(filp, addr, len, pgoff, flags);
	if (addr)
		return addr;

	do_align = filp || (flags & MAP_SHARED);
	info.align_mask = do_align ? (PAGE_MASK & (SHMLBA - 1)) : 0;
	return vm_unmapped_area(&info);
}
