/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Percpu refcounts with RCU protected release operation.
 *
 * Percpu rcuref is similar to percpu refs. However, they are specialized for
 * use cases, where the release of  the object is protected by a RCU grace
 * period.
 *
 * The initial ref is managed by the reclaim logic; so, users do not need to
 * keep track of their initial ref. This is particularly useful, when object's
 * has references active, beyond the release of the initial reference.
 *
 * The current implementation is just a wrapper around the percpu refcount
 * implementation, to reuse the existing percpu and atomic ref switch
 * management. Switching to a standalone implementation might be required
 * if percpuref implementation switches to a non-rcu managed read sections.
 */

#ifndef _LINUX_PERCPU_RCUREFCOUNT_H
#define _LINUX_PERCPU_RCUREFCOUNT_H

#include <linux/percpu-refcount.h>

struct percpu_rcuref;

struct percpu_rcuref {
	struct percpu_ref pcpu_ref;
	struct llist_node node;
};

int __must_check percpu_rcuref_init(struct percpu_rcuref *rcuref,
				 percpu_ref_func_t *release, gfp_t gfp);
int __must_check percpu_rcuref_init_unmanaged(struct percpu_rcuref *rcuref,
				 percpu_ref_func_t *release, gfp_t gfp);
int percpu_rcuref_manage(struct percpu_rcuref *rcuref);
bool percpu_rcuref_is_zero(struct percpu_rcuref *rcuref);
void percpu_rcuref_exit(struct percpu_rcuref *rcuref);

/**
 * percpu_rcuref_get_many - increment a percpu rcuref count
 * @rcuref: percpu_rcuref to get
 * @nr: number of references to get
 *
 * Analogous to percpu_ref_get_many().
 */
static inline void percpu_rcuref_get_many(struct percpu_rcuref *rcuref, unsigned long nr)
{
	percpu_ref_get_many(&rcuref->pcpu_ref, nr);
}

/**
 * percpu_rcuref_get - increment a percpu rcuref count
 * @rcuref: percpu_rcuref to get
 *
 * Analogous to percpu_ref_get().
 *
 */
static inline void percpu_rcuref_get(struct percpu_rcuref *rcuref)
{
	percpu_rcuref_get_many(rcuref, 1);
}

/**
 * percpu_rcuref_tryget_many - try to increment a percpu rcuref count
 * @rcuref: percpu_rcuref to try-get
 * @nr: number of references to get
 *
 * Increment a percpu rcuref count  by @nr unless its count already reached zero.
 * Returns %true on success; %false on failure.
 *
 */
static inline bool percpu_rcuref_tryget_many(struct percpu_rcuref *rcuref,
					  unsigned long nr)
{
	return percpu_ref_tryget_many(&rcuref->pcpu_ref, nr);
}

/**
 * percpu_rcuref_tryget - try to increment a percpu rcuref count
 * @rcuref: percpu_rcuref to try-get
 *
 * Increment a percpu rcurefcount unless its count already reached zero.
 * Returns %true on success; %false on failure.
 *
 */
static inline bool percpu_rcuref_tryget(struct percpu_rcuref *rcuref)
{
	return percpu_rcuref_tryget_many(rcuref, 1);
}

/**
 * percpu_rcuref_put_many - decrement a percpu rcuref count
 * @rcuref: percpu_rcuref to put
 * @nr: number of references to put
 *
 * Decrement the refcount, and if 0, call the release function (which was passed
 * to percpu_rcuref_init())
 */
static inline void percpu_rcuref_put_many(struct percpu_rcuref *rcuref, unsigned long nr)
{
	percpu_ref_put_many(&rcuref->pcpu_ref, nr);
}

/**
 * percpu_rcuref_put - decrement a percpu rcuref count
 * @rcuref: percpu_rcuref to put
 *
 * Decrement the refcount, and if 0, call the release function (which was passed
 * to percpu_ref_init())
 */
static inline void percpu_rcuref_put(struct percpu_rcuref *rcuref)
{
	percpu_rcuref_put_many(rcuref, 1);
}
#endif
