// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel assisted compare-and-swap for pre-v9 sparc.
 */

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>

#include <asm/ptrace.h>
#include <asm/cas_32.h>
#include <asm/cpu_type.h>

raw_spinlock_t sparc32_atomic_locks[SPARC32_ATOMIC_LOCKS] = {
	[0 ... (SPARC32_ATOMIC_LOCKS - 1)] =
		__RAW_SPIN_LOCK_UNLOCKED(sparc32_atomic_locks)
};

/* Report errors in the carry bit as the syscall path does, then step over
 * the trapping instruction.
 */
static void cas_return(struct pt_regs *regs, unsigned long value, bool error)
{
	regs->u_regs[UREG_I0] = value;
	if (error)
		regs->psr |= PSR_C;
	else
		regs->psr &= ~PSR_C;

	regs->pc = regs->npc;
	regs->npc = regs->npc + 4;
}

/* Trap type SP_TRAP_CAS; the ABI is in
 * Documentation/arch/sparc/cas-trap.rst.
 */
asmlinkage void handle_sparc32_cas(struct pt_regs *regs, unsigned long pc,
				   unsigned long npc, unsigned long psr)
{
	u32 __user *uaddr = (u32 __user *)regs->u_regs[UREG_I0];
	u32 oldval = regs->u_regs[UREG_I1];
	u32 newval = regs->u_regs[UREG_I2];
	raw_spinlock_t *lock;
	unsigned long flags;
	u32 val;
	int err;

	if (unlikely((unsigned long)uaddr & 3)) {
		cas_return(regs, EINVAL, true);
		return;
	}

	if (unlikely(!access_ok(uaddr, sizeof(u32)))) {
		cas_return(regs, EFAULT, true);
		return;
	}

	if (sparc32_has_casa) {
		/* No lock needed: casa is atomic against userspace's own. */
		if (likely(!__sparc32_casa_user(uaddr, oldval, newval, &val)))
			cas_return(regs, val, false);
		else
			cas_return(regs, EFAULT, true);
		return;
	}

	lock = sparc32_atomic_lock(uaddr);

retry:
	/* A fault must not take the mmap lock under a raw spinlock, so make
	 * it -EFAULT here and fault the page in after dropping the lock.
	 */
	raw_spin_lock_irqsave(lock, flags);
	pagefault_disable();

	err = __get_user(val, uaddr);
	if (!err && val == oldval)
		err = __put_user(newval, uaddr);

	pagefault_enable();
	raw_spin_unlock_irqrestore(lock, flags);

	if (likely(!err)) {
		cas_return(regs, val, false);
		return;
	}

	/* fault_in_writeable() may modify the target memory. */
	if (fault_in_safe_writeable((char __user *)uaddr, sizeof(u32))) {
		cas_return(regs, EFAULT, true);
		return;
	}

	goto retry;
}
