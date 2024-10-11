// SPDX-License-Identifier: GPL-2.0
/*
 * shstk.c - Intel shadow stack support
 *
 * Copyright (c) 2021, Intel Corporation.
 * Yu-cheng Yu <yu-cheng.yu@intel.com>
 */

#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/compat.h>
#include <linux/sizes.h>
#include <linux/user.h>
#include <linux/syscalls.h>
#include <asm/shstk.h>
#include <linux/usershstk.h>

#define SHSTK_ENTRY_SIZE sizeof(void *)

bool cpu_supports_shadow_stack(void)
{
	return arch_cpu_supports_shadow_stack();
}

bool is_shstk_enabled(struct task_struct *task)
{
	return arch_is_shstk_enabled(task);
}

void set_shstk_base_size(struct task_struct *task, unsigned long base,
			unsigned long size)
{
	arch_set_shstk_base_size(task, base, size);
}

void get_shstk_base_size(struct task_struct *task, unsigned long *base,
			unsigned long *size)
{
	arch_get_shstk_base_size(task, base, size);
}

void set_shstk_ptr_and_enable(unsigned long ssp)
{
	arch_set_shstk_ptr_and_enable(ssp);
}

void set_thread_shstk_status(bool enable)
{
	arch_set_thread_shstk_status(enable);
}

int create_rstor_token(unsigned long ssp, unsigned long *token_addr)
{
	return arch_create_rstor_token(ssp, token_addr);
}

unsigned long adjust_shstk_size(unsigned long size)
{
	if (size)
		return PAGE_ALIGN(size);

	return PAGE_ALIGN(min_t(unsigned long long, rlimit(RLIMIT_STACK), SZ_4G));
}

void unmap_shadow_stack(u64 base, u64 size)
{
	int r;

	r = vm_munmap(base, size);

	/*
	 * mmap_write_lock_killable() failed with -EINTR. This means
	 * the process is about to die and have it's MM cleaned up.
	 * This task shouldn't ever make it back to userspace. In this
	 * case it is ok to leak a shadow stack, so just exit out.
	 */
	if (r == -EINTR)
		return;

	/*
	 * For all other types of vm_munmap() failure, either the
	 * system is out of memory or there is bug.
	 */
	WARN_ON_ONCE(r);
}

/*
 * VM_SHADOW_STACK will have a guard page. This helps userspace protect
 * itself from attacks. The reasoning is as follows:
 *
 * The shadow stack pointer(SSP) is moved by CALL, RET, and INCSSPQ. The
 * INCSSP instruction can increment the shadow stack pointer. It is the
 * shadow stack analog of an instruction like:
 *
 *   addq $0x80, %rsp
 *
 * However, there is one important difference between an ADD on %rsp
 * and INCSSP. In addition to modifying SSP, INCSSP also reads from the
 * memory of the first and last elements that were "popped". It can be
 * thought of as acting like this:
 *
 * READ_ONCE(ssp);       // read+discard top element on stack
 * ssp += nr_to_pop * 8; // move the shadow stack
 * READ_ONCE(ssp-8);     // read+discard last popped stack element
 *
 * The maximum distance INCSSP can move the SSP is 2040 bytes, before
 * it would read the memory. Therefore a single page gap will be enough
 * to prevent any operation from shifting the SSP to an adjacent stack,
 * since it would have to land in the gap at least once, causing a
 * fault.
 */
unsigned long alloc_shstk(unsigned long addr, unsigned long size,
				 unsigned long token_offset, bool set_res_tok)
{
	int flags = MAP_ANONYMOUS | MAP_PRIVATE;

	flags |= IS_ENABLED(CONFIG_X86_64) ? MAP_ABOVE4G : 0;

	struct mm_struct *mm = current->mm;
	unsigned long mapped_addr, unused;

	if (addr)
		flags |= MAP_FIXED_NOREPLACE;

	mmap_write_lock(mm);
	mapped_addr = do_mmap(NULL, addr, size, PROT_READ, flags,
			      VM_SHADOW_STACK | VM_WRITE, 0, &unused, NULL);
	mmap_write_unlock(mm);

	if (!set_res_tok || IS_ERR_VALUE(mapped_addr))
		goto out;

	if (create_rstor_token(mapped_addr + token_offset, NULL)) {
		vm_munmap(mapped_addr, size);
		return -EINVAL;
	}

out:
	return mapped_addr;
}

void shstk_free(struct task_struct *tsk)
{
	unsigned long base, size;

	if (!cpu_supports_shadow_stack() ||
	    !is_shstk_enabled(current))
		return;

	/*
	 * When fork() with CLONE_VM fails, the child (tsk) already has a
	 * shadow stack allocated, and exit_thread() calls this function to
	 * free it.  In this case the parent (current) and the child share
	 * the same mm struct.
	 */
	if (!tsk->mm || tsk->mm != current->mm)
		return;

	get_shstk_base_size(tsk, &base, &size);
	/*
	 * If shstk->base is NULL, then this task is not managing its
	 * own shadow stack (CLONE_VFORK). So skip freeing it.
	 */
	if (!base)
		return;

	/*
	 * shstk->base is NULL for CLONE_VFORK child tasks, and so is
	 * normal. But size = 0 on a shstk->base is not normal and
	 * indicated an attempt to free the thread shadow stack twice.
	 * Warn about it.
	 */
	if (WARN_ON(!size))
		return;

	unmap_shadow_stack(base, size);

	set_shstk_base_size(tsk, 0, 0);
}

SYSCALL_DEFINE3(map_shadow_stack, unsigned long, addr, unsigned long, size, unsigned int, flags)
{
	bool set_tok = flags & SHADOW_STACK_SET_TOKEN;
	unsigned long aligned_size;

	if (!cpu_supports_shadow_stack())
		return -EOPNOTSUPP;

	if (flags & ~SHADOW_STACK_SET_TOKEN)
		return -EINVAL;

	/* If there isn't space for a token */
	if (set_tok && size < SHSTK_ENTRY_SIZE)
		return -ENOSPC;

	if (addr && (addr & (PAGE_SIZE - 1)))
		return -EINVAL;

	if (IS_ENABLED(CONFIG_X86_64) &&
		addr && addr < SZ_4G)
		return -ERANGE;

	/*
	 * An overflow would result in attempting to write the restore token
	 * to the wrong location. Not catastrophic, but just return the right
	 * error code and block it.
	 */
	aligned_size = PAGE_ALIGN(size);
	if (aligned_size < size)
		return -EOVERFLOW;

	return alloc_shstk(addr, aligned_size, size, set_tok);
}

int shstk_setup(void)
{
	struct thread_shstk *shstk = &current->thread.shstk;
	unsigned long addr, size;

	/* Already enabled */
	if (is_shstk_enabled(current))
		return 0;

	/* Also not supported for 32 bit */
	if (!cpu_supports_shadow_stack() ||
		(IS_ENABLED(CONFIG_X86_64) && in_ia32_syscall()))
		return -EOPNOTSUPP;

	size = adjust_shstk_size(0);
	addr = alloc_shstk(0, size, 0, false);
	if (IS_ERR_VALUE(addr))
		return PTR_ERR((void *)addr);

	set_shstk_ptr_and_enable(addr + size);
	set_shstk_base_size(current, addr, size);

	set_thread_shstk_status(true);

	return 0;
}

unsigned long shstk_alloc_thread_stack(struct task_struct *tsk, unsigned long clone_flags,
				       unsigned long stack_size)
{
	struct thread_shstk *shstk = &tsk->thread.shstk;
	unsigned long addr, size;

	if (!cpu_supports_shadow_stack())
		return -EOPNOTSUPP;

	/*
	 * If shadow stack is not enabled on the new thread, skip any
	 * switch to a new shadow stack.
	 */
	if (!is_shstk_enabled(tsk))
		return 0;

	/*
	 * For CLONE_VFORK the child will share the parents shadow stack.
	 * Make sure to clear the internal tracking of the thread shadow
	 * stack so the freeing logic run for child knows to leave it alone.
	 */
	if (clone_flags & CLONE_VFORK) {
		set_shstk_base_size(tsk, 0, 0);
		return 0;
	}

	/*
	 * For !CLONE_VM the child will use a copy of the parents shadow
	 * stack.
	 */
	if (!(clone_flags & CLONE_VM))
		return 0;

	size = adjust_shstk_size(stack_size);
	addr = alloc_shstk(0, size, 0, false);
	if (IS_ERR_VALUE(addr))
		return addr;

	set_shstk_base_size(tsk, addr, size);

	return addr + size;
}
