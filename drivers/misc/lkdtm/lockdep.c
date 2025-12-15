// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Google LLC
 *
 * Tests related to lockdep warnings.
 */
#include "lkdtm.h"
#include <linux/cleanup.h>
#include <linux/irqflags.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>

static DEFINE_SPINLOCK(lock_A);
static DEFINE_SPINLOCK(lock_B);

/* For "WARNING: possible circular locking dependency detected". */
static void lkdtm_LOCKDEP_CIRCULAR_LOCK(void)
{
	scoped_guard(spinlock, &lock_A)
		scoped_guard(spinlock, &lock_B) {}
	scoped_guard(spinlock, &lock_B)
		scoped_guard(spinlock, &lock_A) {}
}

/* For "WARNING: possible recursive locking detected". */
static void lkdtm_LOCKDEP_RECURSIVE_LOCK(void)
{
	guard(spinlock)(&lock_A);
	guard(spinlock)(&lock_A);
}

/* For "WARNING: inconsistent lock state". */
static void lkdtm_LOCKDEP_INCONSISTENT_LOCK(void)
{
	lockdep_softirq_enter();
	scoped_guard(spinlock, &lock_A) {}
	lockdep_softirq_exit();

	scoped_guard(spinlock, &lock_A) {}
}

/* For "WARNING: Nested lock was not taken". */
static void lkdtm_LOCKDEP_NESTED_LOCK_NOT_HELD(void)
{
	spin_lock_nest_lock(&lock_B, &lock_A);
}

/* For "WARNING: bad unlock balance detected!". */
static void lkdtm_LOCKDEP_BAD_UNLOCK_BALANCE(void)
{
	spin_unlock(&lock_A);
}

/* For "WARNING: held lock freed!". */
static void lkdtm_LOCKDEP_HELD_LOCK_FREED(void)
{
	spin_lock(&lock_A);
	spin_lock_init(&lock_A);
}

/* For "WARNING: lock held when returning to user space!". */
static void lkdtm_LOCKDEP_HELD_LOCK(void)
{
	spin_lock(&lock_A);
}

/* For "WARNING: suspicious RCU usage". */
static void lkdtm_LOCKDEP_SUSPICIOUS_RCU(void)
{
	struct srcu_struct srcu;
	void __rcu *res = NULL;
	int idx;

	init_srcu_struct(&srcu);

	idx = srcu_read_lock(&srcu);
	rcu_dereference(res);
	srcu_read_unlock(&srcu, idx);

	cleanup_srcu_struct(&srcu);
}

static struct crashtype crashtypes[] = {
	CRASHTYPE(LOCKDEP_CIRCULAR_LOCK),
	CRASHTYPE(LOCKDEP_RECURSIVE_LOCK),
	CRASHTYPE(LOCKDEP_INCONSISTENT_LOCK),
	CRASHTYPE(LOCKDEP_NESTED_LOCK_NOT_HELD),
	CRASHTYPE(LOCKDEP_BAD_UNLOCK_BALANCE),
	CRASHTYPE(LOCKDEP_HELD_LOCK_FREED),
	CRASHTYPE(LOCKDEP_HELD_LOCK),
	CRASHTYPE(LOCKDEP_SUSPICIOUS_RCU),
};

struct crashtype_category lockdep_crashtypes = {
	.crashtypes = crashtypes,
	.len	    = ARRAY_SIZE(crashtypes),
};
