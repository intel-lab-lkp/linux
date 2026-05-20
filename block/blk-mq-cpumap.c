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

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		num = cpumask_weight_and(mask, housekeeping_cpumask(HK_TYPE_IO_QUEUE));
	else
		num = cpumask_weight(mask);

	return min_not_zero(num, max_queues);
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

static bool blk_mq_validate(struct blk_mq_queue_map *qmap,
			    const unsigned long *active_hctx,
			    const struct cpumask *online_mask)
{
	/*
	 * Verify if the mapping is usable when housekeeping
	 * configuration is enabled
	 */
	for (int queue = 0; queue < qmap->nr_queues; queue++) {
		int cpu;

		if (test_bit(queue, active_hctx)) {
			/*
			 * This hctx has at least one online CPU thus it
			 * is able to serve any assigned isolated CPU.
			 */
			continue;
		}

		/*
		 * There is no housekeeping online CPU for this hctx, all
		 * good as long as all non-housekeeping CPUs are also
		 * offline.
		 */
		for_each_cpu(cpu, online_mask) {
			if (qmap->mq_map[cpu] != qmap->queue_offset + queue)
				continue;

			pr_warn("Unable to create a usable CPU-to-queue mapping with the given constraints\n");
			return false;
		}
	}

	return true;
}

static void blk_mq_map_fallback(struct blk_mq_queue_map *qmap)
{
	unsigned int cpu;

	/*
	 * Map all CPUs to the first hctx of this specific map to ensure
	 * at least one online CPU is serving it, respecting the map's
	 * boundaries so secondary maps do not route into the default map.
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
	cpumask_var_t online_mask;

	active_hctx = bitmap_zalloc(qmap->nr_queues, GFP_KERNEL);
	if (!active_hctx)
		goto fallback;

	if (!alloc_cpumask_var(&online_mask, GFP_KERNEL))
		goto free_fallback_hctx;

	/*
	 * Snapshot online CPUs to prevent TOCTOU races between the
	 * mapping phase and the validation phase.
	 */
	cpumask_copy(online_mask, cpu_online_mask);

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		constraint = housekeeping_cpumask(HK_TYPE_IO_QUEUE);
	else
		constraint = cpu_possible_mask;

	/* Map CPUs to the hardware contexts (hctx) */
	masks = group_mask_cpus_evenly(qmap->nr_queues, constraint, &nr_masks);
	if (!masks)
		goto free_fallback;

	/*
	 * Iterate directly over the generated CPU masks.
	 * Calculate the final, highest hardware queue index that maps to this
	 * mask. This skips all intermediate overwrites and safely evaluates
	 * active_hctx only for queues that survive the mapping.
	 */
	for (unsigned int idx = 0; idx < nr_masks; idx++) {
		bool active = false;
		queue = qmap->nr_queues - 1 -
			((qmap->nr_queues - 1 - idx) % nr_masks);

		for_each_cpu(cpu, &masks[idx]) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;

			if (!active && cpumask_test_cpu(cpu, online_mask)) {
				__set_bit(queue, active_hctx);
				active = true;
			}
		}
	}

	/*
	 * If all CPUs in the generated masks are offline, the active_hctx
	 * bitmap will be empty. Attempting to route unassigned CPUs to an
	 * empty bitmap will map them out-of-bounds. Fall back instead.
	 */
	if (bitmap_empty(active_hctx, qmap->nr_queues))
		goto free_fallback;

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = find_first_bit(active_hctx, qmap->nr_queues);
	for_each_cpu_andnot(cpu, cpu_possible_mask, constraint) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = find_next_bit_wrap(active_hctx, qmap->nr_queues, queue + 1);
	}

	if (!blk_mq_validate(qmap, active_hctx, online_mask))
		goto free_fallback;

	kfree(masks);
	free_cpumask_var(online_mask);
	bitmap_free(active_hctx);

	return;

free_fallback:
	kfree(masks);
	free_cpumask_var(online_mask);
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
	cpumask_var_t mask, online_mask;
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

	if (!alloc_cpumask_var(&online_mask, GFP_KERNEL))
		goto free_fallback_mask;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		constraint = housekeeping_cpumask(HK_TYPE_IO_QUEUE);
	else
		constraint = cpu_possible_mask;

	/*
	 * Snapshot online CPUs to prevent TOCTOU races between the
	 * mapping phase and the validation phase.
	 */
	cpumask_copy(online_mask, cpu_online_mask);

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
		if (cpumask_test_cpu(cpu, online_mask) &&
			cpumask_test_cpu(cpu, constraint)) {
			queue = qmap->mq_map[cpu] - qmap->queue_offset;
			__set_bit(queue, active_hctx);
		}
	}

	/*
	 * If all CPUs assigned to this map are offline, the bitmap will
	 * be empty. Fall back instead of routing out of bounds.
	 */
	if (bitmap_empty(active_hctx, qmap->nr_queues))
		goto free_fallback;

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = find_first_bit(active_hctx, qmap->nr_queues);
	for_each_cpu_andnot(cpu, cpu_possible_mask, mask) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = find_next_bit_wrap(active_hctx, qmap->nr_queues, queue + 1);
	}

	if (!blk_mq_validate(qmap, active_hctx, online_mask))
		goto free_fallback;

	bitmap_free(active_hctx);
	free_cpumask_var(mask);
	free_cpumask_var(online_mask);

	return;

free_fallback:
	free_cpumask_var(online_mask);
free_fallback_mask:
	bitmap_free(active_hctx);
	free_cpumask_var(mask);

fallback:
	blk_mq_map_fallback(qmap);
	return;

free_map_software:
	free_cpumask_var(online_mask);
	free_cpumask_var(mask);
	bitmap_free(active_hctx);
map_software:
	blk_mq_map_queues(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);
