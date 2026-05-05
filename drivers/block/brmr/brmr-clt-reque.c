// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "brmr-clt.h"
#include "rmr.h"
#include "rmr-pool.h"

MODULE_AUTHOR("The RMR and BRMR developers");
MODULE_VERSION(BRMR_VER_STRING);
MODULE_DESCRIPTION("BRMR Block Device using RMR cluster");
MODULE_LICENSE("GPL");

static inline void brmr_requeue(struct brmr_queue *q)
{
	if (WARN_ON(!q->hctx))
		return;

	/* We can come here from interrupt, thus async=true */
	blk_mq_run_hw_queue(q->hctx, true);
}

/**
 * requeue implementation as used by ibnbd
 */

void brmr_init_cpu_qlists(struct brmr_cpu_qlist __percpu *cpu_queues)
{
	unsigned int cpu;
	struct brmr_cpu_qlist *cpu_q;

	for_each_possible_cpu(cpu) {
		cpu_q = per_cpu_ptr(cpu_queues, cpu);

		cpu_q->cpu = cpu;
		INIT_LIST_HEAD(&cpu_q->requeue_list);
		spin_lock_init(&cpu_q->requeue_lock);
	}
}

/**
 * brmr_get_cpu_qlist() - finds a list with HW queues to be requeued
 *
 * Description:
 *     Each CPU has a list of HW queues, which needs to be requeed.  If a list
 *     is not empty - it is marked with a bit.  This function finds first
 *     set bit in a bitmap and returns corresponding CPU list.
 */
static struct brmr_cpu_qlist *
brmr_get_cpu_qlist(struct brmr_clt_pool *pool, int cpu)
{
	int bit;

	/* First half */
	bit = find_next_bit(pool->cpu_queues_bm, nr_cpu_ids, cpu);
	if (bit < nr_cpu_ids) {
		return per_cpu_ptr(pool->cpu_queues, bit);
	} else if (cpu != 0) {
		/* Second half */
		bit = find_next_bit(pool->cpu_queues_bm, cpu, 0);
		if (bit < cpu)
			return per_cpu_ptr(pool->cpu_queues, bit);
	}

	return NULL;
}

static inline int nxt_cpu(int cpu)
{
	return (cpu + 1) % nr_cpu_ids;
}

/**
 * brmr_requeue_if_needed() - requeue if CPU queue is marked as non empty
 *
 * Description:
 *     Each CPU has it's own list of HW queues, which should be requeued.
 *     Function finds such list with HW queues, takes a list lock, picks up
 *     the first HW queue out of the list and requeues it.
 *
 * Return:
 *     True if the queue was requeued, false otherwise.
 *
 * Context:
 *     Does not matter.
 */
static inline bool brmr_requeue_if_needed(struct brmr_clt_pool *pool)
{
	struct brmr_queue *q = NULL;
	struct brmr_cpu_qlist *cpu_q;
	unsigned long flags;
	int *cpup;

	/*
	 * To keep fairness and not to let other queues starve we always
	 * try to wake up someone else in round-robin manner.  That of course
	 * increases latency but queues always have a chance to be executed.
	 */
	cpup = get_cpu_ptr(pool->cpu_rr);
	for (cpu_q = brmr_get_cpu_qlist(pool, nxt_cpu(*cpup)); cpu_q;
	     cpu_q = brmr_get_cpu_qlist(pool, nxt_cpu(cpu_q->cpu))) {
		if (!spin_trylock_irqsave(&cpu_q->requeue_lock, flags))
			continue;
		if (likely(test_bit(cpu_q->cpu, pool->cpu_queues_bm))) {
			q = list_first_entry_or_null(&cpu_q->requeue_list,
						     typeof(*q), requeue_list);
			if (WARN_ON(!q))
				goto clear_bit;
			list_del_init(&q->requeue_list);
			clear_bit_unlock(0, &q->in_list);

			if (list_empty(&cpu_q->requeue_list)) {
				/* Clear bit if nothing is left */
clear_bit:
				clear_bit(cpu_q->cpu, pool->cpu_queues_bm);
			}
		}
		spin_unlock_irqrestore(&cpu_q->requeue_lock, flags);

		if (q)
			break;
	}

	/**
	 * Saves the CPU that is going to be requeued on the per-cpu var. Just
	 * incrementing it doesn't work because brmr_get_cpu_qlist() will
	 * always return the first CPU with something on the queue list when the
	 * value stored on the var is greater than the last CPU with something
	 * on the list.
	 */
	if (cpu_q)
		*cpup = cpu_q->cpu;
	put_cpu_var(pool->cpu_rr);

	if (q)
		brmr_requeue(q);

	return !!q;
}

/**
 * brmr_requeue_requests() - requeue all queues left in the list if
 *     brmr_clt_pool is idling (there are no requests in-flight).
 *
 * Description:
 *     This function tries to rerun all stopped queues if there are no
 *     requests in-flight anymore.  This function tries to solve an obvious
 *     problem, when number of tags < than number of queues (hctx), which
 *     are stopped and put to sleep.  If last tag, which has been just put,
 *     does not wake up all left queues (hctxs), IO requests hang forever.
 *
 *     That can happen when all number of tags, say N, have been exhausted
 *     from one CPU, and we have many block devices per session, say M.
 *     Each block device has it's own queue (hctx) for each CPU, so eventually
 *     we can put that number of queues (hctxs) to sleep: M x nr_cpu_ids.
 *     If number of tags N < M x nr_cpu_ids finally we will get an IO hang.
 *
 *     To avoid this hang last caller of brmr_put_iu() (last caller is the
 *     one who observes pool->busy == 0) must wake up all remaining queues.
 *
 * Context:
 *     Called from msg_io_conf which in turn is a completion handler
 *     that is called from interupt.
 */
void brmr_requeue_requests(struct brmr_clt_pool *pool)
{
	bool requeued;

	do {
		requeued = brmr_requeue_if_needed(pool);
	} while (atomic_read(&pool->busy) == 0 && requeued);
}

bool brmr_add_to_requeue(struct brmr_clt_pool *pool, struct brmr_queue *q)
{
	struct brmr_cpu_qlist *cpu_q;
	unsigned long flags;
	bool added = true;
	bool need_set;

	cpu_q = get_cpu_ptr(pool->cpu_queues);
	spin_lock_irqsave(&cpu_q->requeue_lock, flags);

	if (likely(!test_and_set_bit_lock(0, &q->in_list))) {
		if (WARN_ON(!list_empty(&q->requeue_list)))
			goto unlock;

		need_set = !test_bit(cpu_q->cpu, pool->cpu_queues_bm);
		if (need_set) {
			set_bit(cpu_q->cpu, pool->cpu_queues_bm);
			/* Paired with brmr_put_iu(). Set a bit first
			 * and then observe the busy counter.
			 */
			smp_mb__before_atomic();
		}
		if (likely(atomic_read(&pool->busy))) {
			list_add_tail(&q->requeue_list, &cpu_q->requeue_list);
		} else {
			/* Very unlikely, but possible: busy counter was
			 * observed as zero.  Drop all bits and return
			 * false to restart the queue by ourselves.
			 */
			if (need_set)
				clear_bit(cpu_q->cpu, pool->cpu_queues_bm);
			clear_bit_unlock(0, &q->in_list);
			added = false;
		}
	}
unlock:
	spin_unlock_irqrestore(&cpu_q->requeue_lock, flags);
	put_cpu_ptr(pool->cpu_queues);

	return added;
}

