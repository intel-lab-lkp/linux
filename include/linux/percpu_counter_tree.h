/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/* SPDX-FileCopyrightText: 2025 Mathieu Desnoyers <mathieu.desnoyers@efficios.com> */

#ifndef _PERCPU_COUNTER_TREE_H
#define _PERCPU_COUNTER_TREE_H

#include <linux/cleanup.h>
#include <linux/preempt.h>
#include <linux/atomic.h>
#include <linux/percpu.h>

#ifdef CONFIG_SMP

struct percpu_counter_tree_level_item {
	atomic_t count;
} ____cacheline_aligned_in_smp;

struct percpu_counter_tree {
	/* Fast-path fields. */
	unsigned int __percpu *level0;
	unsigned int level0_bit_mask;
	union {
		unsigned int *i;
		atomic_t *a;
	} approx_sum;
	int bias;			/* bias for counter_set */

	/* Slow-path fields. */
	struct percpu_counter_tree_level_item *items;
	unsigned int batch_size;
	unsigned int inaccuracy;	/* approximation imprecise within ± inaccuracy */
};

int percpu_counter_tree_init(struct percpu_counter_tree *counter, unsigned int batch_size, gfp_t gfp_flags);
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter);
void percpu_counter_tree_add_slowpath(struct percpu_counter_tree *counter, int inc);
int percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter);
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a, struct percpu_counter_tree *b);
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, int v);
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a, struct percpu_counter_tree *b);
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, int v);
void percpu_counter_tree_set(struct percpu_counter_tree *counter, int v);
unsigned int percpu_counter_tree_inaccuracy(struct percpu_counter_tree *counter);

/* Fast paths */

static inline
int percpu_counter_tree_carry(int orig, int res, int inc, unsigned int bit_mask)
{
	if (inc < 0) {
		inc = -(-inc & ~(bit_mask - 1));
		/*
		 * xor bit_mask: underflow.
		 *
		 * If inc has bit set, decrement an additional bit if
		 * there is _no_ bit transition between orig and res.
		 * Else, inc has bit cleared, decrement an additional
		 * bit if there is a bit transition between orig and
		 * res.
		 */
		if ((inc ^ orig ^ res) & bit_mask)
			inc -= bit_mask;
	} else {
		inc &= ~(bit_mask - 1);
		/*
		 * xor bit_mask: overflow.
		 *
		 * If inc has bit set, increment an additional bit if
		 * there is _no_ bit transition between orig and res.
		 * Else, inc has bit cleared, increment an additional
		 * bit if there is a bit transition between orig and
		 * res.
		 */
		if ((inc ^ orig ^ res) & bit_mask)
			inc += bit_mask;
	}
	return inc;
}

static inline
void percpu_counter_tree_add(struct percpu_counter_tree *counter, int inc)
{
	unsigned int bit_mask = counter->level0_bit_mask, orig, res;

	if (!inc)
		return;
	/* Make sure the fast and slow paths use the same cpu number. */
	guard(migrate)();
	res = this_cpu_add_return(*counter->level0, inc);
	orig = res - inc;
	inc = percpu_counter_tree_carry(orig, res, inc, bit_mask);
	if (!inc)
		return;
	percpu_counter_tree_add_slowpath(counter, inc);
}

static inline
int percpu_counter_tree_approximate_sum(struct percpu_counter_tree *counter)
{
	unsigned int v;

	if (!counter->level0_bit_mask)
		v = READ_ONCE(*counter->approx_sum.i);
	else
		v = atomic_read(counter->approx_sum.a);
	return (int) (v + (unsigned int)READ_ONCE(counter->bias));
}

#else	/* !CONFIG_SMP */

struct percpu_counter_tree {
	atomic_t count;
};

static inline
int percpu_counter_tree_init(struct percpu_counter_tree *counter, unsigned int batch_size, gfp_t gfp_flags)
{
	atomic_set(&counter->count, 0);
	return 0;
}

static inline
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter)
{
}

static inline
int percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter)
{
	return atomic_read(&counter->count);
}

static inline
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a, struct percpu_counter_tree *b)
{
	int count_a = percpu_counter_tree_precise_sum(a),
	    count_b = percpu_counter_tree_precise_sum(b);

	if (count_a == count_b)
		return 0;
	if (count_a < count_b)
		return -1;
	return 1;
}

static inline
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, int v)
{
	int count = percpu_counter_tree_precise_sum(counter);

	if (count == v)
		return 0;
	if (count < v)
		return -1;
	return 1;
}

static inline
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a, struct percpu_counter_tree *b)
{
	return percpu_counter_tree_precise_compare(a, b);
}

static inline
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, int v)
{
	return percpu_counter_tree_precise_compare_value(counter, v);
}

static inline
void percpu_counter_tree_set(struct percpu_counter_tree *counter, int v)
{
	atomic_set(&counter->count, v);
}

static inline
unsigned int percpu_counter_tree_inaccuracy(struct percpu_counter_tree *counter)
{
	return 0;
}

static inline
void percpu_counter_tree_add(struct percpu_counter_tree *counter, int inc)
{
	atomic_add(inc, &counter->count);
}

static inline
int percpu_counter_tree_approximate_sum(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_precise_sum(counter);
}

#endif	/* CONFIG_SMP */

static inline
int percpu_counter_tree_approximate_sum_positive(struct percpu_counter_tree *counter)
{
	int v = percpu_counter_tree_approximate_sum(counter);
	return v > 0 ? v : 0;
}

#endif  /* _PERCPU_COUNTER_TREE_H */
