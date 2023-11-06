// SPDX-License-Identifier: GPL-2.0
/*
 * Out-of-line refcount functions.
 */

#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/bug.h>

#define REFCOUNT_WARN(str)	WARN_ONCE(1, "refcount_t: " str ".\n")

void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t)
{
	refcount_set(r, REFCOUNT_SATURATED);

	switch (t) {
	case REFCOUNT_ADD_NOT_ZERO_OVF:
		REFCOUNT_WARN("saturated; leaking memory");
		break;
	case REFCOUNT_ADD_OVF:
		REFCOUNT_WARN("saturated; leaking memory");
		break;
	case REFCOUNT_ADD_UAF:
		REFCOUNT_WARN("addition on 0; use-after-free");
		break;
	case REFCOUNT_SUB_UAF:
		REFCOUNT_WARN("underflow; use-after-free");
		break;
	case REFCOUNT_DEC_LEAK:
		REFCOUNT_WARN("decrement hit 0; leaking memory");
		break;
	default:
		REFCOUNT_WARN("unknown saturation event!?");
	}
}
EXPORT_SYMBOL(refcount_warn_saturate);

/**
 * refcount_dec_if_one - decrement a refcount if it is 1
 * @r: the refcount
 *
 * No atomic_t counterpart, it attempts a 1 -> 0 transition and returns the
 * success thereof.
 *
 * Like all decrement operations, it provides release memory order and provides
 * a control dependency.
 *
 * It can be used like a try-delete operator; this explicit case is provided
 * and not cmpxchg in generic, because that would allow implementing unsafe
 * operations.
 *
 * Return: true if the resulting refcount is 0, false otherwise
 */
bool refcount_dec_if_one(refcount_t *r)
{
	int val = 1;

	return atomic_try_cmpxchg_release(&r->refs, &val, 0);
}
EXPORT_SYMBOL(refcount_dec_if_one);

/**
 * refcount_dec_not_one - decrement a refcount if it is not 1
 * @r: the refcount
 *
 * No atomic_t counterpart, it decrements unless the value is 1, in which case
 * it will return false.
 *
 * Was often done like: atomic_add_unless(&var, -1, 1)
 *
 * Return: true if the decrement operation was successful, false otherwise
 */
bool refcount_dec_not_one(refcount_t *r)
{
	unsigned int new, val = atomic_read(&r->refs);

	do {
		if (unlikely(val == REFCOUNT_SATURATED))
			return true;

		if (val == 1)
			return false;

		new = val - 1;
		if (new > val) {
			WARN_ONCE(new > val, "refcount_t: underflow; use-after-free.\n");
			return true;
		}

	} while (!atomic_try_cmpxchg_release(&r->refs, &val, new));

	return true;
}
EXPORT_SYMBOL(refcount_dec_not_one);

bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock)
{
	if (refcount_dec_not_one(r))
		return false;

	mutex_lock(lock);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(lock);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(refcount_dec_and_mutex_lock);

bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock)
{
	if (refcount_dec_not_one(r))
		return false;

	spin_lock(lock);
	if (!refcount_dec_and_test(r)) {
		spin_unlock(lock);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(refcount_dec_and_lock);

bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock,
				   unsigned long *flags)
{
	if (refcount_dec_not_one(r))
		return false;

	spin_lock_irqsave(lock, *flags);
	if (!refcount_dec_and_test(r)) {
		spin_unlock_irqrestore(lock, *flags);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(refcount_dec_and_lock_irqsave);
