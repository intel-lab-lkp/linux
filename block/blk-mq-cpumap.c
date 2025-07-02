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

static struct cpumask blk_hk_online_mask;

static unsigned int blk_mq_num_queues(const struct cpumask *mask,
				      unsigned int max_queues)
{
	unsigned int num;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE)) {
		const struct cpumask *hk_mask;
		struct cpumask avail_mask;

		hk_mask = housekeeping_cpumask(HK_TYPE_IO_QUEUE);
		cpumask_and(&avail_mask, mask, hk_mask);

		num = cpumask_weight(&avail_mask);
	} else {
		num = cpumask_weight(mask);
	}

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
	if (housekeeping_enabled(HK_TYPE_IO_QUEUE)) {
		cpumask_and(&blk_hk_online_mask, cpu_online_mask,
			    housekeeping_cpumask(HK_TYPE_IO_QUEUE));
		return &blk_hk_online_mask;
	}

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

static bool blk_mq_hk_validate(struct blk_mq_queue_map *qmap,
			       const struct cpumask *active_hctx)
{
	/*
	 * Verify if the mapping is usable.
	 *
	 * First, mark all hctx which have at least online houskeeping
	 * CPU assigned.
	 */
	for (int queue = 0; queue < qmap->nr_queues; queue++) {
		int cpu;

		if (cpumask_test_cpu(queue, active_hctx)) {
			/*
			 * This htcx has at least one online houskeeping
			 * CPU thus it is able to serve any assigned
			 * isolated CPU.
			 */
			continue;
		}

		/*
		 * There is no online houskeeping CPU for this hctx, all
		 * good as long as all isolated CPUs are also offline.
		 */
		for_each_online_cpu(cpu) {
			if (qmap->mq_map[cpu] != queue)
				continue;

			pr_warn("Unable to create a usable CPU-to-queue mapping with the given constraints\n");
			return false;
		}
	}

	return true;
}

/*
 * blk_mq_map_hk_queues - Create housekeeping CPU to
 *                        hardware queue mapping
 * @qmap:	CPU to hardware queue map
 *
 * Create a housekeeping CPU to hardware queue mapping in @qmap. @qmap
 * contains a valid configuration honoring the isolcpus configuration.
 */
static void blk_mq_map_hk_queues(struct blk_mq_queue_map *qmap)
{
	cpumask_var_t active_hctx __free(free_cpumask_var) = NULL;
	struct cpumask *hk_masks __free(kfree) = NULL;
	const struct cpumask *mask;
	unsigned int queue, cpu, nr_masks;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE))
		mask = housekeeping_cpumask(HK_TYPE_IO_QUEUE);
	else
		goto fallback;

	if (!zalloc_cpumask_var(&active_hctx, GFP_KERNEL))
		goto fallback;

	/* Map housekeeping CPUs to a hctx */
	hk_masks = group_mask_cpus_evenly(qmap->nr_queues, mask, &nr_masks);
	if (!hk_masks)
		goto fallback;

	for (queue = 0; queue < qmap->nr_queues; queue++) {
		unsigned int idx = (qmap->queue_offset + queue) % nr_masks;

		for_each_cpu(cpu, &hk_masks[idx]) {
			qmap->mq_map[cpu] = idx;

			if (cpu_online(cpu))
				cpumask_set_cpu(qmap->mq_map[cpu], active_hctx);
		}
	}

	/* Map isolcpus to hardware context */
	queue = cpumask_first(active_hctx);
	for_each_cpu_andnot(cpu, cpu_possible_mask, mask) {
		qmap->mq_map[cpu] = (qmap->queue_offset + queue) % nr_masks;
		queue = cpumask_next_wrap(queue, active_hctx);
	}

	if (!blk_mq_hk_validate(qmap, active_hctx))
		goto fallback;

	return;

fallback:
	/*
	 * Map all CPUs to the first hctx to ensure at least one online
	 * housekeeping CPU is serving it.
	 */
	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;
}

/*
 * blk_mq_map_hk_irq_queues - Create housekeeping CPU to
 *                            hardware queue mapping
 * @dev:	The device to map queues
 * @qmap:	CPU to hardware queue map
 * @offset:	Queue offset to use for the device
 *
 * Create a housekeeping CPU to hardware queue mapping in @qmap. @qmap
 * contains a valid configuration honoring the isolcpus configuration.
 */
static void blk_mq_map_hk_irq_queues(struct device *dev,
				     struct blk_mq_queue_map *qmap,
				     int offset)
{
	cpumask_var_t active_hctx __free(free_cpumask_var) = NULL;
	cpumask_var_t mask __free(free_cpumask_var) = NULL;
	unsigned int queue, cpu;

	if (!zalloc_cpumask_var(&active_hctx, GFP_KERNEL))
		goto fallback;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		goto fallback;

	/* Map housekeeping CPUs to a hctx */
	for (queue = 0; queue < qmap->nr_queues; queue++) {
		for_each_cpu(cpu, dev->bus->irq_get_affinity(dev, offset + queue)) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;

			cpumask_set_cpu(cpu, mask);
			if (cpu_online(cpu))
				cpumask_set_cpu(qmap->mq_map[cpu], active_hctx);
		}
	}

	/* Map isolcpus to hardware context */
	queue = cpumask_first(active_hctx);
	for_each_cpu_andnot(cpu, cpu_possible_mask, mask) {
		qmap->mq_map[cpu] = qmap->queue_offset + queue;
		queue = cpumask_next_wrap(queue, active_hctx);
	}

	if (!blk_mq_hk_validate(qmap, active_hctx))
		goto fallback;

	return;

fallback:
	/*
	 * Map all CPUs to the first hctx to ensure at least one online
	 * housekeeping CPU is serving it.
	 */
	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;
}

void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	const struct cpumask *masks;
	unsigned int queue, cpu, nr_masks;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE)) {
		blk_mq_map_hk_queues(qmap);
		return;
	}

	masks = group_cpus_evenly(qmap->nr_queues, &nr_masks);
	if (!masks) {
		for_each_possible_cpu(cpu)
			qmap->mq_map[cpu] = qmap->queue_offset;
		return;
	}

	for (queue = 0; queue < qmap->nr_queues; queue++) {
		for_each_cpu(cpu, &masks[queue % nr_masks])
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
	}
	kfree(masks);
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
	const struct cpumask *mask;
	unsigned int queue, cpu;

	if (!dev->bus->irq_get_affinity)
		goto fallback;

	if (housekeeping_enabled(HK_TYPE_IO_QUEUE)) {
		blk_mq_map_hk_irq_queues(dev, qmap, offset);
		return;
	}

	for (queue = 0; queue < qmap->nr_queues; queue++) {
		mask = dev->bus->irq_get_affinity(dev, queue + offset);
		if (!mask)
			goto fallback;

		for_each_cpu(cpu, mask)
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
	}

	return;

fallback:
	blk_mq_map_queues(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);
