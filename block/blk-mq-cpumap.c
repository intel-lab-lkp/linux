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
 * blk_mq_possible_queue_affinity - Return block layer queue affinity
 *
 * Returns an affinity mask that represents the queue-to-CPU mapping
 * requested by the block layer based on possible CPUs.
 * This helper takes isolcpus settings into account.
 */
const struct cpumask *blk_mq_possible_queue_affinity(void)
{
	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		return housekeeping_cpumask(HK_TYPE_IO_QUEUE);

	return cpu_possible_mask;
}
EXPORT_SYMBOL_GPL(blk_mq_possible_queue_affinity);

/**
 * blk_mq_online_queue_affinity - Return block layer queue affinity
 *
 * Returns an affinity mask that represents the queue-to-CPU mapping
 * requested by the block layer based on online CPUs.
 */
const struct cpumask *blk_mq_online_queue_affinity(void)
{
	/*
	 * Return the stable housekeeping mask if enabled. Callers (e.g.,
	 * the IRQ affinity core) are responsible for safely intersecting
	 * this with a local snapshot of the online mask.
	 */
	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		return housekeeping_cpumask(HK_TYPE_IO_QUEUE);

	return cpu_online_mask;
}
EXPORT_SYMBOL_GPL(blk_mq_online_queue_affinity);

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
			    const struct cpumask *active_hctx)
{
	/*
	 * Verify if the mapping is usable when housekeeping
	 * configuration is enabled
	 */

	for (int queue = 0; queue < qmap->nr_queues; queue++) {
		int cpu;

		if (cpumask_test_cpu(queue, active_hctx)) {
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
		for_each_online_cpu(cpu) {
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
	 * Map all CPUs to the first hctx to ensure at least one online
	 * CPU is serving it.
	 */
	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;
}

void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	struct cpumask *masks __free(kfree) = NULL;
	const struct cpumask *constraint;
	unsigned int queue, cpu, nr_masks;
	cpumask_var_t active_hctx;

	if (!zalloc_cpumask_var(&active_hctx, GFP_KERNEL))
		goto fallback;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		constraint = housekeeping_cpumask(HK_TYPE_IO_QUEUE);
	else
		constraint = cpu_possible_mask;

	/* Map CPUs to the hardware contexts (hctx) */
	masks = group_mask_cpus_evenly(qmap->nr_queues, constraint, &nr_masks);
	if (!masks)
		goto free_fallback;

	for (queue = 0; queue < qmap->nr_queues; queue++) {
		unsigned int idx = (qmap->queue_offset + queue) % nr_masks;

		for_each_cpu(cpu, &masks[idx]) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;

			if (cpu_online(cpu))
				cpumask_set_cpu(queue, active_hctx);
		}
	}

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = cpumask_first(active_hctx);
	for_each_cpu_andnot(cpu, cpu_possible_mask, constraint) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = cpumask_next_wrap(queue, active_hctx);
	}

	if (!blk_mq_validate(qmap, active_hctx))
		goto free_fallback;

	free_cpumask_var(active_hctx);

	return;

free_fallback:
	free_cpumask_var(active_hctx);

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
	cpumask_var_t active_hctx, mask;
	unsigned int queue, cpu;

	if (!dev->bus->irq_get_affinity)
		goto fallback;

	if (!zalloc_cpumask_var(&active_hctx, GFP_KERNEL))
		goto fallback;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		free_cpumask_var(active_hctx);
		goto fallback;
	}

	/* Map CPUs to the hardware contexts (hctx) */
	for (queue = 0; queue < qmap->nr_queues; queue++) {
		const struct cpumask *affinity_mask;

		affinity_mask = dev->bus->irq_get_affinity(dev, offset + queue);
		if (!affinity_mask)
			goto free_fallback;

		for_each_cpu(cpu, affinity_mask) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;

			cpumask_set_cpu(cpu, mask);
			if (cpu_online(cpu))
				cpumask_set_cpu(queue, active_hctx);
		}
	}

	/* Map any unassigned CPU evenly to the hardware contexts (hctx) */
	queue = cpumask_first(active_hctx);
	for_each_cpu_andnot(cpu, cpu_possible_mask, mask) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = cpumask_next_wrap(queue, active_hctx);
	}

	if (!blk_mq_validate(qmap, active_hctx))
		goto free_fallback;

	free_cpumask_var(active_hctx);
	free_cpumask_var(mask);

	return;

free_fallback:
	free_cpumask_var(active_hctx);
	free_cpumask_var(mask);

fallback:
	blk_mq_map_fallback(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);
