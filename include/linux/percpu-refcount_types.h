/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PERCPU_REFCOUNT_TYPES_H
#define _LINUX_PERCPU_REFCOUNT_TYPES_H

#include <linux/atomic.h>
#include <linux/types.h>

struct percpu_ref;
typedef void (percpu_ref_func_t)(struct percpu_ref *);

/* flags set in the lower bits of percpu_ref->percpu_count_ptr */
enum {
	__PERCPU_REF_ATOMIC	= 1LU << 0,	/* operating in atomic mode */
	__PERCPU_REF_DEAD	= 1LU << 1,	/* (being) killed */
	__PERCPU_REF_ATOMIC_DEAD = __PERCPU_REF_ATOMIC | __PERCPU_REF_DEAD,

	__PERCPU_REF_FLAG_BITS	= 2,
};

/* @flags for percpu_ref_init() */
enum {
	/*
	 * Start w/ ref == 1 in atomic mode.  Can be switched to percpu
	 * operation using percpu_ref_switch_to_percpu().  If initialized
	 * with this flag, the ref will stay in atomic mode until
	 * percpu_ref_switch_to_percpu() is invoked on it.
	 * Implies ALLOW_REINIT.
	 */
	PERCPU_REF_INIT_ATOMIC	= 1 << 0,

	/*
	 * Start dead w/ ref == 0 in atomic mode.  Must be revived with
	 * percpu_ref_reinit() before used.  Implies INIT_ATOMIC and
	 * ALLOW_REINIT.
	 */
	PERCPU_REF_INIT_DEAD	= 1 << 1,

	/*
	 * Allow switching from atomic mode to percpu mode.
	 */
	PERCPU_REF_ALLOW_REINIT	= 1 << 2,
};

struct percpu_ref_data {
	atomic_long_t		count;
	percpu_ref_func_t	*release;
	percpu_ref_func_t	*confirm_switch;
	bool			force_atomic:1;
	bool			allow_reinit:1;
	struct rcu_head		rcu;
	struct percpu_ref	*ref;
};

struct percpu_ref {
	/*
	 * The low bit of the pointer indicates whether the ref is in percpu
	 * mode; if set, then get/put will manipulate the atomic_t.
	 */
	unsigned long		percpu_count_ptr;

	/*
	 * 'percpu_ref' is often embedded into user structure, and only
	 * 'percpu_count_ptr' is required in fast path, move other fields
	 * into 'percpu_ref_data', so we can reduce memory footprint in
	 * fast path.
	 */
	struct percpu_ref_data  *data;
};

#endif
