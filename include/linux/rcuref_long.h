/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_RCUREF_LONG_H
#define _LINUX_RCUREF_LONG_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/limits.h>
#include <linux/lockdep.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/rcuref.h>

#ifdef CONFIG_64BIT
#define RCUREF_LONG_ONEREF	0x0000000000000000U
#define RCUREF_LONG_MAXREF	0x7FFFFFFFFFFFFFFFU
#define RCUREF_LONG_SATURATED   0xA000000000000000U
#define RCUREF_LONG_RELEASED    0xC000000000000000U
#define RCUREF_LONG_DEAD	0xE000000000000000U
#define RCUREF_LONG_NOREF	0xFFFFFFFFFFFFFFFFU
#else
#define RCUREF_LONG_ONEREF	RCUREF_ONEREF
#define RCUREF_LONG_MAXREF	RCUREF_MAXREF
#define RCUREF_LONG_SATURATED	RCUREF_SATURATED
#define RCUREF_LONG_RELEASED	RCUREF_RELEASED
#define RCUREF_LONG_DEAD	RCUREF_DEAD
#define RCUREF_LONG_NOREF	RCUREF_NOREF
#endif

/**
 * rcuref_long_init - Initialize a rcuref reference count with the given reference count
 * @ref:	Pointer to the reference count
 * @cnt:	The initial reference count typically '1'
 */
static inline void rcuref_long_init(rcuref_long_t *ref, unsigned long cnt)
{
	atomic_long_set(&ref->refcnt, cnt - 1);
}

/**
 * rcuref_long_read - Read the number of held reference counts of a rcuref
 * @ref:	Pointer to the reference count
 *
 * Return: The number of held references (0 ... N)
 */
static inline unsigned long rcuref_long_read(rcuref_long_t *ref)
{
	unsigned long c = atomic_long_read(&ref->refcnt);

	/* Return 0 if within the DEAD zone. */
	return c >= RCUREF_LONG_RELEASED ? 0 : c + 1;
}

__must_check bool rcuref_long_get_slowpath(rcuref_long_t *ref);

/**
 * rcuref_long_get - Acquire one reference on a rcuref reference count
 * @ref:	Pointer to the reference count
 *
 * Similar to atomic_long_inc_not_zero() but saturates at RCUREF_LONG_MAXREF.
 *
 * Provides no memory ordering, it is assumed the caller has guaranteed the
 * object memory to be stable (RCU, etc.). It does provide a control dependency
 * and thereby orders future stores. See documentation in lib/rcuref.c
 *
 * Return:
 *	False if the attempt to acquire a reference failed. This happens
 *	when the last reference has been put already
 *
 *	True if a reference was successfully acquired
 */
static inline __must_check bool rcuref_long_get(rcuref_long_t *ref)
{
	/*
	 * Unconditionally increase the reference count. The saturation and
	 * dead zones provide enough tolerance for this.
	 */
	if (likely(!atomic_long_add_negative_relaxed(1, &ref->refcnt)))
		return true;

	/* Handle the cases inside the saturation and dead zones */
	return rcuref_long_get_slowpath(ref);
}

__must_check bool rcuref_long_put_slowpath(rcuref_long_t *ref);

/*
 * Internal helper. Do not invoke directly.
 */
static __always_inline __must_check bool __rcuref_long_put(rcuref_long_t *ref)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() && preemptible(),
			 "suspicious rcuref_put_rcusafe() usage");
	/*
	 * Unconditionally decrease the reference count. The saturation and
	 * dead zones provide enough tolerance for this.
	 */
	if (likely(!atomic_long_add_negative_release(-1, &ref->refcnt)))
		return false;

	/*
	 * Handle the last reference drop and cases inside the saturation
	 * and dead zones.
	 */
	return rcuref_long_put_slowpath(ref);
}

/**
 * rcuref_long_put_rcusafe -- Release one reference for a rcuref reference count RCU safe
 * @ref:	Pointer to the reference count
 *
 * Provides release memory ordering, such that prior loads and stores are done
 * before, and provides an acquire ordering on success such that free()
 * must come after.
 *
 * Can be invoked from contexts, which guarantee that no grace period can
 * happen which would free the object concurrently if the decrement drops
 * the last reference and the slowpath races against a concurrent get() and
 * put() pair. rcu_read_lock()'ed and atomic contexts qualify.
 *
 * Return:
 *	True if this was the last reference with no future references
 *	possible. This signals the caller that it can safely release the
 *	object which is protected by the reference counter.
 *
 *	False if there are still active references or the put() raced
 *	with a concurrent get()/put() pair. Caller is not allowed to
 *	release the protected object.
 */
static inline __must_check bool rcuref_long_put_rcusafe(rcuref_long_t *ref)
{
	return __rcuref_long_put(ref);
}

/**
 * rcuref_long_put -- Release one reference for a rcuref reference count
 * @ref:	Pointer to the reference count
 *
 * Can be invoked from any context.
 *
 * Provides release memory ordering, such that prior loads and stores are done
 * before, and provides an acquire ordering on success such that free()
 * must come after.
 *
 * Return:
 *
 *	True if this was the last reference with no future references
 *	possible. This signals the caller that it can safely schedule the
 *	object, which is protected by the reference counter, for
 *	deconstruction.
 *
 *	False if there are still active references or the put() raced
 *	with a concurrent get()/put() pair. Caller is not allowed to
 *	deconstruct the protected object.
 */
static inline __must_check bool rcuref_long_put(rcuref_long_t *ref)
{
	bool released;

	preempt_disable();
	released = __rcuref_long_put(ref);
	preempt_enable();
	return released;
}

#endif
