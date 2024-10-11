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
#include <asm/msr.h>
#include <asm/fpu/xstate.h>
#include <asm/fpu/types.h>
#include <asm/shstk.h>
#include <asm/special_insns.h>
#include <asm/fpu/api.h>
#include <asm/prctl.h>
#include <linux/usershstk.h>

#define SS_FRAME_SIZE 8

static bool features_enabled(unsigned long features)
{
	return current->thread.features & features;
}

static void features_set(unsigned long features)
{
	current->thread.features |= features;
}

static void features_clr(unsigned long features)
{
	current->thread.features &= ~features;
}

bool arch_cpu_supports_shadow_stack(void)
{
	return cpu_feature_enabled(X86_FEATURE_USER_SHSTK);
}

bool arch_is_shstk_enabled(struct task_struct *task)
{
	return features_enabled(ARCH_SHSTK_SHSTK);
}

void arch_set_shstk_base_size(struct task_struct *task, unsigned long base,
			unsigned long size)
{
	struct thread_shstk *shstk = &task->thread.shstk;

	shstk->base = base;
	shstk->size = size;
}

void arch_get_shstk_base_size(struct task_struct *task, unsigned long *base,
			unsigned long *size)
{
	struct thread_shstk *shstk = &task->thread.shstk;

	*base = shstk->base;
	*size = shstk->size;
}


void arch_set_shstk_ptr_and_enable(unsigned long ssp)
{
	fpregs_lock_and_load();
	wrmsrl(MSR_IA32_PL3_SSP, ssp);
	wrmsrl(MSR_IA32_U_CET, CET_SHSTK_EN);
	fpregs_unlock();
}

void arch_set_thread_shstk_status(bool enable)
{
	if (enable)
		features_set(ARCH_SHSTK_SHSTK);
	else
		features_clr(ARCH_SHSTK_SHSTK);
}

/*
 * Create a restore token on the shadow stack.  A token is always 8-byte
 * and aligned to 8.
 */
int arch_create_rstor_token(unsigned long ssp, unsigned long *token_addr)
{
	unsigned long addr;

	/* Token must be aligned */
	if (!IS_ALIGNED(ssp, 8))
		return -EINVAL;

	addr = ssp - SS_FRAME_SIZE;

	/*
	 * SSP is aligned, so reserved bits and mode bit are a zero, just mark
	 * the token 64-bit.
	 */
	ssp |= BIT(0);

	if (write_user_shstk_64((u64 __user *)addr, (u64)ssp))
		return -EFAULT;

	if (token_addr)
		*token_addr = addr;

	return 0;
}

void reset_thread_features(void)
{
	memset(&current->thread.shstk, 0, sizeof(struct thread_shstk));
	current->thread.features = 0;
	current->thread.features_locked = 0;
}

static unsigned long get_user_shstk_addr(void)
{
	unsigned long long ssp;

	fpregs_lock_and_load();

	rdmsrl(MSR_IA32_PL3_SSP, ssp);

	fpregs_unlock();

	return ssp;
}

#define SHSTK_DATA_BIT BIT(63)

static int put_shstk_data(u64 __user *addr, u64 data)
{
	if (WARN_ON_ONCE(data & SHSTK_DATA_BIT))
		return -EINVAL;

	/*
	 * Mark the high bit so that the sigframe can't be processed as a
	 * return address.
	 */
	if (write_user_shstk_64(addr, data | SHSTK_DATA_BIT))
		return -EFAULT;
	return 0;
}

static int get_shstk_data(unsigned long *data, unsigned long __user *addr)
{
	unsigned long ldata;

	if (unlikely(get_user(ldata, addr)))
		return -EFAULT;

	if (!(ldata & SHSTK_DATA_BIT))
		return -EINVAL;

	*data = ldata & ~SHSTK_DATA_BIT;

	return 0;
}

static int shstk_push_sigframe(unsigned long *ssp)
{
	unsigned long target_ssp = *ssp;

	/* Token must be aligned */
	if (!IS_ALIGNED(target_ssp, 8))
		return -EINVAL;

	*ssp -= SS_FRAME_SIZE;
	if (put_shstk_data((void __user *)*ssp, target_ssp))
		return -EFAULT;

	return 0;
}

static int shstk_pop_sigframe(unsigned long *ssp)
{
	struct vm_area_struct *vma;
	unsigned long token_addr;
	bool need_to_check_vma;
	int err = 1;

	/*
	 * It is possible for the SSP to be off the end of a shadow stack by 4
	 * or 8 bytes. If the shadow stack is at the start of a page or 4 bytes
	 * before it, it might be this case, so check that the address being
	 * read is actually shadow stack.
	 */
	if (!IS_ALIGNED(*ssp, 8))
		return -EINVAL;

	need_to_check_vma = PAGE_ALIGN(*ssp) == *ssp;

	if (need_to_check_vma)
		mmap_read_lock_killable(current->mm);

	err = get_shstk_data(&token_addr, (unsigned long __user *)*ssp);
	if (unlikely(err))
		goto out_err;

	if (need_to_check_vma) {
		vma = find_vma(current->mm, *ssp);
		if (!vma || !(vma->vm_flags & VM_SHADOW_STACK)) {
			err = -EFAULT;
			goto out_err;
		}

		mmap_read_unlock(current->mm);
	}

	/* Restore SSP aligned? */
	if (unlikely(!IS_ALIGNED(token_addr, 8)))
		return -EINVAL;

	/* SSP in userspace? */
	if (unlikely(token_addr >= TASK_SIZE_MAX))
		return -EINVAL;

	*ssp = token_addr;

	return 0;
out_err:
	if (need_to_check_vma)
		mmap_read_unlock(current->mm);
	return err;
}

int setup_signal_shadow_stack(struct ksignal *ksig)
{
	void __user *restorer = ksig->ka.sa.sa_restorer;
	unsigned long ssp;
	int err;

	if (!cpu_feature_enabled(X86_FEATURE_USER_SHSTK) ||
	    !features_enabled(ARCH_SHSTK_SHSTK))
		return 0;

	if (!restorer)
		return -EINVAL;

	ssp = get_user_shstk_addr();
	if (unlikely(!ssp))
		return -EINVAL;

	err = shstk_push_sigframe(&ssp);
	if (unlikely(err))
		return err;

	/* Push restorer address */
	ssp -= SS_FRAME_SIZE;
	err = write_user_shstk_64((u64 __user *)ssp, (u64)restorer);
	if (unlikely(err))
		return -EFAULT;

	fpregs_lock_and_load();
	wrmsrl(MSR_IA32_PL3_SSP, ssp);
	fpregs_unlock();

	return 0;
}

int restore_signal_shadow_stack(void)
{
	unsigned long ssp;
	int err;

	if (!cpu_feature_enabled(X86_FEATURE_USER_SHSTK) ||
	    !features_enabled(ARCH_SHSTK_SHSTK))
		return 0;

	ssp = get_user_shstk_addr();
	if (unlikely(!ssp))
		return -EINVAL;

	err = shstk_pop_sigframe(&ssp);
	if (unlikely(err))
		return err;

	fpregs_lock_and_load();
	wrmsrl(MSR_IA32_PL3_SSP, ssp);
	fpregs_unlock();

	return 0;
}

static int wrss_control(bool enable)
{
	u64 msrval;

	if (!cpu_feature_enabled(X86_FEATURE_USER_SHSTK))
		return -EOPNOTSUPP;

	/*
	 * Only enable WRSS if shadow stack is enabled. If shadow stack is not
	 * enabled, WRSS will already be disabled, so don't bother clearing it
	 * when disabling.
	 */
	if (!features_enabled(ARCH_SHSTK_SHSTK))
		return -EPERM;

	/* Already enabled/disabled? */
	if (features_enabled(ARCH_SHSTK_WRSS) == enable)
		return 0;

	fpregs_lock_and_load();
	rdmsrl(MSR_IA32_U_CET, msrval);

	if (enable) {
		features_set(ARCH_SHSTK_WRSS);
		msrval |= CET_WRSS_EN;
	} else {
		features_clr(ARCH_SHSTK_WRSS);
		if (!(msrval & CET_WRSS_EN))
			goto unlock;

		msrval &= ~CET_WRSS_EN;
	}

	wrmsrl(MSR_IA32_U_CET, msrval);

unlock:
	fpregs_unlock();

	return 0;
}

static int shstk_disable(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_USER_SHSTK))
		return -EOPNOTSUPP;

	/* Already disabled? */
	if (!features_enabled(ARCH_SHSTK_SHSTK))
		return 0;

	fpregs_lock_and_load();
	/* Disable WRSS too when disabling shadow stack */
	wrmsrl(MSR_IA32_U_CET, 0);
	wrmsrl(MSR_IA32_PL3_SSP, 0);
	fpregs_unlock();

	shstk_free(current);
	features_clr(ARCH_SHSTK_SHSTK | ARCH_SHSTK_WRSS);

	return 0;
}

long shstk_prctl(struct task_struct *task, int option, unsigned long arg2)
{
	unsigned long features = arg2;

	if (option == ARCH_SHSTK_STATUS) {
		return put_user(task->thread.features, (unsigned long __user *)arg2);
	}

	if (option == ARCH_SHSTK_LOCK) {
		task->thread.features_locked |= features;
		return 0;
	}

	/* Only allow via ptrace */
	if (task != current) {
		if (option == ARCH_SHSTK_UNLOCK && IS_ENABLED(CONFIG_CHECKPOINT_RESTORE)) {
			task->thread.features_locked &= ~features;
			return 0;
		}
		return -EINVAL;
	}

	/* Do not allow to change locked features */
	if (features & task->thread.features_locked)
		return -EPERM;

	/* Only support enabling/disabling one feature at a time. */
	if (hweight_long(features) > 1)
		return -EINVAL;

	if (option == ARCH_SHSTK_DISABLE) {
		if (features & ARCH_SHSTK_WRSS)
			return wrss_control(false);
		if (features & ARCH_SHSTK_SHSTK)
			return shstk_disable();
		return -EINVAL;
	}

	/* Handle ARCH_SHSTK_ENABLE */
	if (features & ARCH_SHSTK_SHSTK)
		return shstk_setup();
	if (features & ARCH_SHSTK_WRSS)
		return wrss_control(true);
	return -EINVAL;
}

int shstk_update_last_frame(unsigned long val)
{
	unsigned long ssp;

	if (!features_enabled(ARCH_SHSTK_SHSTK))
		return 0;

	ssp = get_user_shstk_addr();
	return write_user_shstk_64((u64 __user *)ssp, (u64)val);
}

bool shstk_is_enabled(void)
{
	return features_enabled(ARCH_SHSTK_SHSTK);
}
