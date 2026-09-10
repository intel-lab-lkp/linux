// SPDX-License-Identifier: GPL-2.0
/*
 * CPU <-> hardware queue mapping helpers
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */
#include <linux/kernel.h>
#include <linux/threads.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/group_cpus.h>
#include <linux/device/bus.h>
#include <linux/sched/isolation.h>

#include "blk.h"
#include "blk-mq.h"

static unsigned int blk_mq_num_queues(const struct cpumask *mask,
				      unsigned int max_queues)
{
	unsigned int num;

	if (housekeeping_enabled(HK_TYPE_MANAGED_IRQ_STRICT))
		num = cpumask_weight_and(mask, housekeeping_cpumask(HK_TYPE_MANAGED_IRQ_STRICT));
	else
		num = cpumask_weight(mask);
	/*
	 * Ensure that a count of zero does not inadvertently result in
	 * allocating the maximum number of queues.
	 */
	return min_not_zero(num ?: 1U, max_queues);
}

/**
 * blk_mq_num_possible_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of possible CPUs. This helper
 * takes isolcpus settings into account.
 */
unsigned int blk_mq_num_possible_queues(unsigned int max_queues)
{
	return blk_mq_num_queues(cpu_possible_mask, max_queues);
}
EXPORT_SYMBOL_GPL(blk_mq_num_possible_queues);

/**
 * blk_mq_num_online_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of online CPUs. This helper
 * takes isolcpus settings into account.
 */
unsigned int blk_mq_num_online_queues(unsigned int max_queues)
{
	return blk_mq_num_queues(cpu_online_mask, max_queues);
}
EXPORT_SYMBOL_GPL(blk_mq_num_online_queues);

static void blk_mq_map_fallback(struct blk_mq_queue_map *qmap)
{
	unsigned int cpu;

	/*
	 * Map all CPUs to the first hctx of this specific map, respecting
	 * the map's boundaries so secondary maps do not route into the default map.
	 */
	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = qmap->queue_offset;
}

void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	struct cpumask *masks;
	const struct cpumask *constraint;
	unsigned int queue, cpu, nr_masks;
	unsigned long *active_hctx;

	active_hctx = bitmap_zalloc(qmap->nr_queues, GFP_KERNEL);
	if (!active_hctx)
		goto fallback;

	if (housekeeping_enabled(HK_TYPE_MANAGED_IRQ_STRICT))
		constraint = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ_STRICT);
	else
		constraint = cpu_possible_mask;

	/* Map CPUs to the hardware contexts (hctx) */
	masks = group_mask_cpus_evenly(qmap->nr_queues, constraint, &nr_masks);
	if (!masks)
		goto free_fallback_hctx;

	/*
	 * Iterate directly over the generated CPU masks.
	 * Calculate the final, highest hardware queue index that maps to this
	 * mask. This skips all intermediate overwrites and safely evaluates
	 * active_hctx only for queues that survive the mapping.
	 */
	for (unsigned int idx = 0; idx < nr_masks; idx++) {
		queue = qmap->nr_queues - 1 -
			((qmap->nr_queues - 1 - idx) % nr_masks);

		for_each_cpu(cpu, &masks[idx])
			qmap->mq_map[cpu] = qmap->queue_offset + queue;

		__set_bit(queue, active_hctx);
	}

	/*
	 * If the active_hctx bitmap is empty, attempting to route unassigned
	 * CPUs will map them out-of-bounds. Fall back instead.
	 */
	if (bitmap_empty(active_hctx, qmap->nr_queues))
		goto free_fallback;

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = find_first_bit(active_hctx, qmap->nr_queues);
	for_each_cpu_andnot(cpu, cpu_possible_mask, constraint) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = find_next_bit_wrap(active_hctx, qmap->nr_queues, queue + 1);
	}

	kfree(masks);
	bitmap_free(active_hctx);

	return;

free_fallback:
	kfree(masks);
free_fallback_hctx:
	bitmap_free(active_hctx);

fallback:
	blk_mq_map_fallback(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_queues);

/**
 * blk_mq_hw_queue_to_node - Look up the memory node for a hardware queue index
 * @qmap: CPU to hardware queue map.
 * @index: hardware queue index.
 *
 * We have no quick way of doing reverse lookups. This is only used at
 * queue init time, so runtime isn't important.
 */
int blk_mq_hw_queue_to_node(struct blk_mq_queue_map *qmap, unsigned int index)
{
	int i;

	for_each_possible_cpu(i) {
		if (index == qmap->mq_map[i])
			return cpu_to_node(i);
	}

	return NUMA_NO_NODE;
}

/**
 * blk_mq_map_hw_queues - Create CPU to hardware queue mapping
 * @qmap:	CPU to hardware queue map
 * @dev:	The device to map queues
 * @offset:	Queue offset to use for the device
 *
 * Create a CPU to hardware queue mapping in @qmap. The struct bus_type
 * irq_get_affinity callback will be used to retrieve the affinity.
 */
void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct device *dev, unsigned int offset)

{
	cpumask_var_t mask;
	const struct cpumask *constraint;
	unsigned long *active_hctx;
	unsigned int queue, cpu;

	if (!dev->bus->irq_get_affinity)
		goto map_software;

	active_hctx = bitmap_zalloc(qmap->nr_queues, GFP_KERNEL);
	if (!active_hctx)
		goto fallback;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		bitmap_free(active_hctx);
		goto fallback;
	}

	if (housekeeping_enabled(HK_TYPE_MANAGED_IRQ_STRICT))
		constraint = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ_STRICT);
	else
		constraint = cpu_possible_mask;

	/* Map CPUs to the hardware contexts (hctx) */
	for (queue = 0; queue < qmap->nr_queues; queue++) {
		const struct cpumask *affinity_mask;

		affinity_mask = dev->bus->irq_get_affinity(dev, offset + queue);
		if (!affinity_mask)
			goto free_map_software;

		for_each_cpu(cpu, affinity_mask) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
			cpumask_set_cpu(cpu, mask);
		}
	}

	/*
	 * Evaluate active_hctx after mapping to handle overlapping masks.
	 * This ensures queues that were overwritten do not falsely pass validation.
	 */
	for_each_cpu(cpu, mask) {
		if (cpumask_test_cpu(cpu, constraint)) {
			queue = qmap->mq_map[cpu] - qmap->queue_offset;
			__set_bit(queue, active_hctx);
		}
	}

	/*
	 * If no assigned CPU matches the constraint, the active_hctx
	 * bitmap will be empty. Fall back instead of routing out of bounds.
	 */
	if (bitmap_empty(active_hctx, qmap->nr_queues))
		goto free_fallback;

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = find_first_bit(active_hctx, qmap->nr_queues);
	for_each_cpu_andnot(cpu, cpu_possible_mask, mask) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = find_next_bit_wrap(active_hctx, qmap->nr_queues, queue + 1);
	}

	bitmap_free(active_hctx);
	free_cpumask_var(mask);

	return;

free_fallback:
	bitmap_free(active_hctx);
	free_cpumask_var(mask);

fallback:
	blk_mq_map_fallback(qmap);
	return;

free_map_software:
	free_cpumask_var(mask);
	bitmap_free(active_hctx);
map_software:
	blk_mq_map_queues(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);
