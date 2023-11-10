// SPDX-License-Identifier: GPL-2.0

#include <linux/exitz.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/string.h>

#define EZ_MAX_PAGES_ARRAY_COUNT 16
#define EZ_MAX_KMALLOC_PAGES (PAGE_SIZE * 2)
#define EZ_MAX_PAGES_PER_LOOP (EZ_MAX_KMALLOC_PAGES / sizeof(struct page *))

/*
 * Overwrite a range of process memory with zeros (end excluded).
 */
int memz_range(unsigned long start, unsigned long end)
{
	if (end <= start)
		return 0;

	unsigned long nr_pages = (end - 1) / PAGE_SIZE - start / PAGE_SIZE + 1;

	struct page *pages_stack[EZ_MAX_PAGES_ARRAY_COUNT];
	struct page **pages = pages_stack;

	if (nr_pages > EZ_MAX_PAGES_ARRAY_COUNT) {
		/* For reliability, cap kmalloc size */
		pages = kmalloc(min_t(size_t, EZ_MAX_KMALLOC_PAGES,
					sizeof(struct page *) * nr_pages),
				GFP_KERNEL);

		if (!pages)
			return -ENOMEM;
	}

	unsigned long page_address = start & PAGE_MASK;

	while (nr_pages) {
		long pinned_pages = min(nr_pages, EZ_MAX_PAGES_PER_LOOP);

		pinned_pages = pin_user_pages(page_address, pinned_pages, FOLL_WRITE, pages);

		if (pinned_pages <= 0)
			return -EFAULT;

		/* Map and zero each page */
		for (long i = 0; i < pinned_pages; i++) {
			void *kaddr = kmap_local_page(pages[i]);

			memset(kaddr, 0, PAGE_SIZE);

			kunmap_local(kaddr);
		}

		nr_pages -= pinned_pages;
		page_address += pinned_pages * PAGE_SIZE;

		unpin_user_pages_dirty_lock(pages, pinned_pages, 1);
	}

	if (pages != pages_stack)
		kfree(pages);

	return 0;
}

/*
 * Overwrite any memory associated to current process with zeros.
 */
void exit_memz(void)
{
	if (!(current->ezflags & EZ_MEM))
		return;

	struct vm_area_struct *vma;

	VMA_ITERATOR(vmi, current->mm, 0);

	for_each_vma(vmi, vma) {
		memz_range(vma->vm_start, vma->vm_end);
	}
}

/*
 * Overwrite all flagged resources with zeros.
 */
void exit_z(void)
{
	exit_memz();
}

/*
 * Set task_struct flags to zero flagged resources on exit.
 */
void do_exitz(int flags)
{
	current->ezflags = flags;
}

#ifdef CONFIG_EXITZ_SYSCALL
SYSCALL_DEFINE1(exitz, int, flags)
{
	if (flags & ~EZ_FLAGS)
		return -EINVAL;

	do_exitz(flags);
	return 0;
}
#endif
