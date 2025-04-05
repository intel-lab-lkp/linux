/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/* SPDX-FileCopyrightText: 2025 Mathieu Desnoyers <mathieu.desnoyers@efficios.com> */

#ifndef _PERCPU_COUNTER_TREE_H
#define _PERCPU_COUNTER_TREE_H

#include <linux/cleanup.h>
#include <linux/preempt.h>
#include <linux/atomic.h>
#include <linux/percpu.h>

struct percpu_counter_tree_level_item {
	atomic_t count;
} ____cacheline_aligned_in_smp;

struct percpu_counter_tree {
	unsigned int level0_bit_mask;
	unsigned int __percpu *level0;

	unsigned int nr_levels;
	unsigned int nr_cpus;
	unsigned int batch_size;
	struct percpu_counter_tree_level_item *items;
	unsigned int inaccuracy;	/* approximation imprecise within ± inaccuracy */
	int bias;			/* bias for counter_set */
};

int percpu_counter_tree_init(struct percpu_counter_tree *counter, unsigned int batch_size);
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter);
void percpu_counter_tree_add_slowpath(struct percpu_counter_tree *counter, int inc);
int percpu_counter_tree_precise_sum_unbiased(struct percpu_counter_tree *counter);
int percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter);
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *counter, int v);
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *counter, int v);
void percpu_counter_tree_set_bias(struct percpu_counter_tree *counter, int bias);
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
int percpu_counter_tree_approx_sum(struct percpu_counter_tree *counter)
{
	return (int) (atomic_read(&counter->items[counter->nr_cpus - 2].count) +
		      READ_ONCE(counter->bias));
}

#endif  /* _PERCPU_COUNTER_TREE_H */
