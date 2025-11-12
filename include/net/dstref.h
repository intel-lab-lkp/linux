/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_DSTREF_H
#define _NET_DSTREF_H

#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

/**
 * This is required since we can't include dst.h here, in order avoid circular includes between
 * skbuff.h and dst.h.
 */
struct dst_entry;

/**
 * typedef dstref_t - a pointer to a dst which may or may not hold a reference to the dst.
 */
typedef unsigned long __bitwise dstref_t;

/**
 * This bit is used to specify whether or not the dstref object holds a reference to its dst_entry.
 */
#define DSTREF_DST_NOREF       1UL
#define DSTREF_DST_PTRMASK     ~(DSTREF_DST_NOREF)

/**
 * An empty dstref object which does not point to any dst.
 */
#define DSTREF_EMPTY ((__force dstref_t)0UL)

/**
 * A noref variant of an empty dstref object which does not point to any dst.
 */
#define DSTREF_EMPTY_NOREF ((__force dstref_t)DSTREF_DST_NOREF)

/**
 * dst_to_dstref - create a dstref object which holds a reference to the dst.
 * @dst: dst to convert.
 *
 * The provided dst can be NULL, in which case an empty dstref is returned.
 *
 * This function steals the reference on the provided dst, and does not take an extra reference on
 * it.
 *
 * Return: dstref object which points to the given dst and holds a reference to it, or an empty
 * dstref object if dst is NULL.
 */
static inline dstref_t dst_to_dstref(struct dst_entry *dst)
{
	return (__force dstref_t)dst;
}

/**
 * dst_to_dstref_noref - create a dstref pointer which does not hold a reference to the dst.
 * @dst: dst to convert.
 *
 * The provided dst can be NULL, in which case a noref empty dstref is returned.
 *
 * This function must be called within an RCU read-side critical section.
 *
 * Return: dstref object which points to the given dst and does not hold a reference to it, or a
 * noref empty dstref object if dst is NULL.
 */
static inline dstref_t dst_to_dstref_noref(struct dst_entry *dst)
{
	WARN_ON(!rcu_read_lock_held() && !rcu_read_lock_bh_held());
	return (__force dstref_t)((unsigned long)dst | DSTREF_DST_NOREF);
}

/**
 * Is the given dstref object a noref dstref, which doesn't hold a reference to the dst that it
 * points to?
 */
static inline bool dstref_is_noref(dstref_t dstref)
{
	return (__force unsigned long)dstref & DSTREF_DST_NOREF;
}

/*
 * __dstref_dst - get the dst that is pointed at by the given dstref object, without performing
 * safety checks.
 * @dstref: the dstref object to get the dst of.
 *
 * This function returns the dst without performing safety checks.
 * Prefer using dstref_dst instead of using this function.
 *
 * Return: the dst object pointed at by the given dstref object.
 */
static inline struct dst_entry *__dstref_dst(dstref_t dstref)
{
	return (struct dst_entry *)((__force unsigned long)dstref & DSTREF_DST_PTRMASK);
}

/**
 * dstref_dst - get the dst that is pointed at by the given dstref object.
 * @dstref: the dstref object to get the dst of.
 *
 * If the dstref object is noref, this function must be called within an RCU read-side critical
 * section.
 *
 * Return: the dst object pointed at by the given dstref object.
 */
static inline struct dst_entry *dstref_dst(dstref_t dstref)
{
	WARN_ON(dstref_is_noref(dstref) &&
		!rcu_read_lock_held() &&
		!rcu_read_lock_bh_held());
	return __dstref_dst(dstref);
}

#endif /* _NET_DSTREF_H */
