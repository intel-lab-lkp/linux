// SPDX-License-Identifier: GPL-2.0+ OR MIT
// SPDX-FileCopyrightText: 2025 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

/*
 * Split Counters With Binary Tree Approximation Propagation
 *
 * * Propagation diagram when reaching batch size thresholds (± batch size):
 *
 * Example diagram for 8 CPUs:
 *
 * log2(8) = 3 levels
 *
 * At each level, each pair propagates its values to the next level when
 * reaching the batch size thresholds.
 *
 * Counters at levels 0, 1, 2 can be kept on a single byte (±128 range),
 * although it may be relevant to keep them on 32-bit counters for
 * simplicity. (complexity vs memory footprint tradeoff)
 *
 * Counter at level 3 can be kept on a 32-bit counter.
 *
 * Level 0:  0    1    2    3    4    5    6    7
 *           |   /     |   /     |   /     |   /
 *           |  /      |  /      |  /      |  /
 *           | /       | /       | /       | /
 * Level 1:  0         1         2         3
 *           |       /           |       /
 *           |    /              |    /
 *           | /                 | /
 * Level 2:  0                   1
 *           |               /
 *           |         /
 *           |   /
 * Level 3:  0
 *
 * * Approximation inaccuracy:
 *
 * BATCH(level N): Level N batch size.
 *
 * Example for BATCH(level 0) = 32.
 *
 * BATCH(level 0) =  32
 * BATCH(level 1) =  64
 * BATCH(level 2) = 128
 * BATCH(level N) = BATCH(level 0) * 2^N
 *
 *            per-counter     global
 *            inaccuracy      inaccuracy
 * Level 0:   [ -32 ..  +31]  ±256  (8 * 32)
 * Level 1:   [ -64 ..  +63]  ±256  (4 * 64)
 * Level 2:   [-128 .. +127]  ±256  (2 * 128)
 * Total:      ------         ±768  (log2(nr_cpu_ids) * BATCH(level 0) * nr_cpu_ids)
 *
 * -----
 *
 * Approximate Sum Carry Propagation
 *
 * Let's define a number of counter bits for each level, e.g.:
 *
 * log2(BATCH(level 0)) = log2(32) = 5
 *
 *               nr_bit        value_mask                      range
 * Level 0:      5 bits        v                             0 ..  +31
 * Level 1:      1 bit        (v & ~((1UL << 5) - 1))        0 ..  +63
 * Level 2:      1 bit        (v & ~((1UL << 6) - 1))        0 .. +127
 * Level 3:     25 bits       (v & ~((1UL << 7) - 1))        0 .. 2^32-1
 *
 * Note: Use a full 32-bit per-cpu counter at level 0 to allow precise sum.
 *
 * Note: Use cacheline aligned counters at levels above 0 to prevent false sharing.
 *       If memory footprint is an issue, a specialized allocator could be used
 *       to eliminate padding.
 *
 * Example with expanded values:
 *
 * counter_add(counter, inc):
 *
 *         if (!inc)
 *                 return;
 *
 *         res = percpu_add_return(counter @ Level 0, inc);
 *         orig = res - inc;
 *         if (inc < 0) {
 *                 inc = -(-inc & ~0b00011111);  // Clear used bits
 *                 // xor bit 5: underflow
 *                 if ((inc ^ orig ^ res) & 0b00100000)
 *                         inc -= 0b00100000;
 *         } else {
 *                 inc &= ~0b00011111;           // Clear used bits
 *                 // xor bit 5: overflow
 *                 if ((inc ^ orig ^ res) & 0b00100000)
 *                         inc += 0b00100000;
 *         }
 *         if (!inc)
 *                 return;
 *
 *         res = atomic_add_return(counter @ Level 1, inc);
 *         orig = res - inc;
 *         if (inc < 0) {
 *                 inc = -(-inc & ~0b00111111);  // Clear used bits
 *                 // xor bit 6: underflow
 *                 if ((inc ^ orig ^ res) & 0b01000000)
 *                         inc -= 0b01000000;
 *         } else {
 *                 inc &= ~0b00111111;           // Clear used bits
 *                 // xor bit 6: overflow
 *                 if ((inc ^ orig ^ res) & 0b01000000)
 *                         inc += 0b01000000;
 *         }
 *         if (!inc)
 *                 return;
 *
 *         res = atomic_add_return(counter @ Level 2, inc);
 *         orig = res - inc;
 *         if (inc < 0) {
 *                 inc = -(-inc & ~0b01111111);  // Clear used bits
 *                 // xor bit 7: underflow
 *                 if ((inc ^ orig ^ res) & 0b10000000)
 *                         inc -= 0b10000000;
 *         } else {
 *                 inc &= ~0b01111111;           // Clear used bits
 *                 // xor bit 7: overflow
 *                 if ((inc ^ orig ^ res) & 0b10000000)
 *                         inc += 0b10000000;
 *         }
 *         if (!inc)
 *                 return;
 *
 *         atomic_add(counter @ Level 3, inc);
 */

#include <linux/percpu_counter_tree.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/math.h>

int percpu_counter_tree_init(struct percpu_counter_tree *counter, unsigned int batch_size)
{
	/* Batch size must be power of 2 */
	if (!batch_size || (batch_size & (batch_size - 1)))
		return -EINVAL;
	counter->nr_levels = get_count_order(nr_cpu_ids);
	counter->nr_cpus = 1UL << counter->nr_levels;
	counter->batch_size = batch_size;
	counter->level0_bit_mask = 1UL << get_count_order(batch_size);
	counter->inaccuracy = counter->nr_levels * batch_size * counter->nr_cpus;
	counter->bias = 0;
	counter->level0 = alloc_percpu(unsigned int);
	if (!counter->level0)
		return -ENOMEM;
	counter->items = kzalloc(counter->nr_cpus - 1 *
				 sizeof(struct percpu_counter_tree_level_item),
				 GFP_KERNEL);
	if (!counter->items) {
		free_percpu(counter->level0);
		return -ENOMEM;
	}
	return 0;
}

void percpu_counter_tree_destroy(struct percpu_counter_tree *counter)
{
	free_percpu(counter->level0);
	kfree(counter->items);
}

/* Called with migration disabled. */
void percpu_counter_tree_add_slowpath(struct percpu_counter_tree *counter, int inc)
{
	struct percpu_counter_tree_level_item *item = counter->items;
	unsigned int level_items = counter->nr_cpus >> 1;
	unsigned int level, nr_levels = counter->nr_levels;
	unsigned int bit_mask = counter->level0_bit_mask;
	unsigned int cpu = smp_processor_id();

	for (level = 1; level < nr_levels; level++) {
		atomic_t *count = &item[cpu & (level_items - 1)].count;
		unsigned int orig, res;

		bit_mask <<= 1;
		res = atomic_add_return_relaxed(inc, count);
		orig = res - inc;
		inc = percpu_counter_tree_carry(orig, res, inc, bit_mask);
		item += level_items;
		level_items >>= 1;
		if (!inc)
			return;
	}
	atomic_add(inc, &item->count);
}

/*
 * Precise sum.
 * Keep "int" counters per-cpu, and perform the sum of all per-cpu
 * counters.
 */
int percpu_counter_tree_precise_sum_unbiased(struct percpu_counter_tree *counter)
{
	unsigned int sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += *per_cpu_ptr(counter->level0, cpu);
	return (int) sum;
}

int percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_precise_sum_unbiased(counter) + READ_ONCE(counter->bias);
}

/*
 * Do an approximate comparison of a counter against a given value.
 * Return 1 if the value is greater than counter,
 * Return -1 if the value lower than counter,
 * Return 0 if the value is within the inaccuracy range of the counter.
 */
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *counter, int v)
{
	int count = percpu_counter_tree_approx_sum(counter);

	if (abs(v - count) <= counter->inaccuracy)
		return 0;
	if (v > count)
		return 1;
	return -1;
}

/*
 * Compare counter against a given value.
 * Return 1 if the value is greater than counter,
 * Return -1 if the value lower than counter,
 * Return 0 if the value is equal to the counter.
 */
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *counter, int v)
{
	int count = percpu_counter_tree_approx_sum(counter);

	if (abs(v - count) <= counter->inaccuracy)
		count = percpu_counter_tree_precise_sum(counter);
	if (v > count)
		return 1;
	if (v < count)
		return -1;
	return 0;
}

void percpu_counter_tree_set_bias(struct percpu_counter_tree *counter, int bias)
{
	WRITE_ONCE(counter->bias, bias);
}

void percpu_counter_tree_set(struct percpu_counter_tree *counter, int v)
{
	percpu_counter_tree_set_bias(counter,
				     v - percpu_counter_tree_precise_sum_unbiased(counter));
}

unsigned int percpu_counter_tree_inaccuracy(struct percpu_counter_tree *counter)
{
	return counter->inaccuracy;
}
