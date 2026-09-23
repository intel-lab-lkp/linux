/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FUTEX_H
#define _ASM_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>
#include <asm/cas_32.h>
#include <asm/cpu_type.h>

/* These share the compare-and-swap trap's lock array and must hash
 * identically to it; see asm/cas_32.h.
 */

static inline int sparc32_futex_op(int op, int oparg, u32 oldval, u32 *newval)
{
	switch (op) {
	case FUTEX_OP_SET:
		*newval = oparg;
		break;
	case FUTEX_OP_ADD:
		*newval = oldval + oparg;
		break;
	case FUTEX_OP_OR:
		*newval = oldval | oparg;
		break;
	case FUTEX_OP_ANDN:
		*newval = oldval & ~oparg;
		break;
	case FUTEX_OP_XOR:
		*newval = oldval ^ oparg;
		break;
	default:
		return -ENOSYS;
	}

	return 0;
}

static inline int
arch_futex_atomic_op_inuser(int op, int oparg, int *oval, u32 __user *uaddr)
{
	raw_spinlock_t *lock;
	unsigned long flags;
	u32 oldval, newval, prev;
	int ret = 0;

	if (sparc32_has_casa) {
		if (!access_ok(uaddr, sizeof(u32)))
			return -EFAULT;

		if (unlikely(get_user(oldval, uaddr) != 0))
			return -EFAULT;

		/* Retry until the word still holds what newval was computed
		 * from, making the read-modify-write one atomic step.
		 */
		for (;;) {
			ret = sparc32_futex_op(op, oparg, oldval, &newval);
			if (ret)
				return ret;

			if (unlikely(__sparc32_casa_user(uaddr, oldval,
							 newval, &prev) != 0))
				return -EFAULT;

			if (likely(prev == oldval))
				break;

			oldval = prev;
		}

		*oval = oldval;
		return 0;
	}

	lock = sparc32_atomic_lock(uaddr);
	raw_spin_lock_irqsave(lock, flags);

	if (unlikely(get_user(oldval, uaddr) != 0)) {
		ret = -EFAULT;
		goto out;
	}

	ret = sparc32_futex_op(op, oparg, oldval, &newval);
	if (ret)
		goto out;

	if (unlikely(put_user(newval, uaddr) != 0))
		ret = -EFAULT;

out:
	raw_spin_unlock_irqrestore(lock, flags);

	if (!ret)
		*oval = oldval;

	return ret;
}

static inline int
futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
			      u32 oldval, u32 newval)
{
	raw_spinlock_t *lock;
	unsigned long flags;
	u32 val;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	/* casa is atomic against userspace's own; a lock of ours is not. */
	if (sparc32_has_casa) {
		if (__sparc32_casa_user(uaddr, oldval, newval, &val))
			return -EFAULT;
		*uval = val;
		return 0;
	}

	lock = sparc32_atomic_lock(uaddr);
	raw_spin_lock_irqsave(lock, flags);

	if (unlikely(get_user(val, uaddr) != 0)) {
		raw_spin_unlock_irqrestore(lock, flags);
		return -EFAULT;
	}

	if (val == oldval && unlikely(put_user(newval, uaddr) != 0)) {
		raw_spin_unlock_irqrestore(lock, flags);
		return -EFAULT;
	}

	raw_spin_unlock_irqrestore(lock, flags);

	*uval = val;
	return 0;
}

#endif /* _ASM_FUTEX_H */
