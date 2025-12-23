// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit userspace memory allocation resource management.
 */
#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/kthread.h>
#include <linux/mm.h>

int kunit_attach_mm(void)
{
	struct mm_struct *mm;

	if (current->mm)
		return 0;

	/* arch_pick_mmap_layout() is only sane with MMU systems. */
	if (!IS_ENABLED(CONFIG_MMU))
		return -EINVAL;

	mm = mm_alloc();
	if (!mm)
		return -ENOMEM;

	/* Define the task size. */
	mm->task_size = TASK_SIZE;

	/* Make sure we can allocate new VMAs. */
	arch_pick_mmap_layout(mm, &current->signal->rlim[RLIMIT_STACK]);

	/* Attach the mm. It will be cleaned up when the process dies. */
	kthread_take_mm(mm);

	return 0;
}
EXPORT_SYMBOL_GPL(kunit_attach_mm);

unsigned long kunit_vm_mmap(struct kunit *test, struct file *file,
			    unsigned long addr, unsigned long len,
			    unsigned long prot, unsigned long flag,
			    unsigned long offset)
{
	int err;

	err = kunit_attach_mm();
	if (err)
		return err;

	return vm_mmap(file, addr, len, prot, flag, offset);
}
EXPORT_SYMBOL_GPL(kunit_vm_mmap);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
