// SPDX-License-Identifier: GPL-2.0
/*
 * Arm CLA driver - domain scheduling
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/rhashtable.h>
#include <linux/mmu_context.h>

#include "arm-cla.h"

static const struct rhashtable_params ctxs_rht_params = {
	.head_offset = offsetof(struct cla_ctx, node),
	.key_offset = offsetof(struct cla_ctx, key),
	.key_len = sizeof(((struct cla_ctx *)0)->key),
	.automatic_shrinking = true,
};

static int __cla_domain_switch_ctx(struct cla_domain *domain,
				   struct cla_ctx *prev_ctx,
				   struct cla_ctx *next_ctx)
{
	int ret = 0;
	unsigned int i;
	struct cla_dev *dev;
	struct cla_call_on_cpu *call;

	if (!prev_ctx && !next_ctx)
		return 0;

	for (i = 0; i < domain->nr_devs; i++) {
		dev = domain->devs[i];
		call = &dev->call;

		call->sched.prev_ctx = prev_ctx;
		call->sched.next_ctx = next_ctx;
		call->sched.ctx_id = i;
		call->ret = 0;

		mutex_lock(&dev->lock);
		if (dev->worker && !dev->broken)
			kthread_queue_work(dev->worker, &call->switch_ctx);
		mutex_unlock(&dev->lock);
	}

	for (i = 0; i < domain->nr_devs; i++) {
		dev = domain->devs[i];
		call = &dev->call;

		down_read(&dev->worker_sem);
		kthread_flush_work(&call->switch_ctx);
		up_read(&dev->worker_sem);

		/*
		 * kthread worker and kthread_flush_work() both take the worker
		 * lock to respectively write and read the current_work,
		 * providing the desired memory ordering for ret.
		 */
		if (call->ret)
			ret = call->ret;
	}

	return ret;
}

static int cla_domain_switch_ctx(struct cla_domain *domain,
				 struct cla_ctx *prev_ctx,
				 struct cla_ctx *next_ctx)
{
	int ret;

	/* Remove prev_ctx from all devices in the domain */
	ret = __cla_domain_switch_ctx(domain, prev_ctx, NULL);
	if (ret) {
		mmgrab(prev_ctx->key.mm);
		goto err_kill_domain;
	}

	if (prev_ctx)
		arm64_mm_context_put(prev_ctx->key.mm);
	if (next_ctx) {
		next_ctx->asid = arm64_mm_context_get(next_ctx->key.mm);
		if (next_ctx->asid == 0) {
			pr_err("cla: out of pinned ASIDs\n");
			ret = -ENOSPC;
			goto err_kill_domain;
		}
	}

	/* Install next_ctx into all devices in the domain */
	ret = __cla_domain_switch_ctx(domain, NULL, next_ctx);
	if (ret) {
		mmgrab(next_ctx->key.mm);
		goto err_kill_domain;
	}
	return 0;

err_kill_domain:
	/* Hardware failure, the domain is dead */
	cla_domain_set_broken(domain);
	return ret;
}

/*
 * This should only be called within the context of the device's worker, and
 * so is bound to the cla's local cpu. The worker ensures serialization of
 * invocations for the same cla. cla_dev_switch_ctx() may be running
 * concurrently on other cpus for other clas in the same domain.
 */
static int __cla_dev_switch_ctx(struct cla_dev *dev)
{
	struct cla_ctx *prev_ctx, *next_ctx;
	unsigned long ctx_id;
	int ret;

	if (WARN_ON(smp_processor_id() != dev->cpu))
		return -EINVAL;

	prev_ctx = dev->call.sched.prev_ctx;
	next_ctx = dev->call.sched.next_ctx;
	ctx_id = dev->call.sched.ctx_id;

	/* Save state for outgoing ctx. */
	if (prev_ctx) {
		/*
		 * Since we're not holding the domain lock during this check, we
		 * may be saving a dying context, but it's only an optimisation.
		 */
		bool do_save = !cla_ctx_is_dying(prev_ctx);

		ret = cla_regs_switch_out(dev, prev_ctx->regs[ctx_id], do_save);
		if (ret) {
			cla_err(dev, "failed to remove regs: %d\n", ret);
			return ret;
		}

		ret = cla_mtc_uninstall(dev);
		if (ret) {
			cla_err(dev, "failed to remove mm: %d\n", ret);
			return ret;
		}
	}

	/* Restore state for incoming ctx. */
	if (next_ctx) {
		ret = cla_mtc_install(dev, next_ctx->key.mm->pgd, next_ctx->asid);
		if (ret) {
			cla_err(dev, "failed to setup mm: %d\n", ret);
			return ret;
		}

		ret = cla_regs_switch_in(dev, next_ctx->regs[ctx_id]);
		if (ret) {
			cla_err(dev, "failed to setup regs: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

void cla_dev_switch_ctx(struct kthread_work *work)
{
	struct cla_call_on_cpu *call =
		container_of(work, struct cla_call_on_cpu, switch_ctx);
	struct cla_dev *dev = container_of(call, struct cla_dev, call);

	call->ret = __cla_dev_switch_ctx(dev);
	if (call->ret) {
		mutex_lock(&dev->lock);
		dev->broken = true;
		mutex_unlock(&dev->lock);
	}
}

static inline struct cla_ctx *cla_domain_get_next_ctx(struct cla_domain *domain)
{
	return list_first_entry_or_null(&domain->queued_ctxs, struct cla_ctx,
					queue_node);
}

static inline struct cla_ctx *cla_domain_get_prev_ctx(struct cla_domain *domain,
						      struct cla_ctx *next_ctx)
{
	if (domain->assigned_ctx && (next_ctx || cla_ctx_is_dying(domain->assigned_ctx)))
		return domain->assigned_ctx;
	return NULL;
}

static void cla_vma_zap_domain(struct vm_area_struct *vma, struct cla_domain *domain)
{
	unsigned long vma_start = vma->vm_pgoff;
	unsigned long vma_end = vma_start + vma_pages(vma);
	unsigned long domain_start = domain->pg_offset;
	unsigned long domain_end = domain_start + domain->nr_devs;
	unsigned long start = max(vma_start, domain_start);
	unsigned long end = min(vma_end, domain_end);
	unsigned long addr;
	unsigned long size;

	if (start >= end)
		return;

	addr = vma->vm_start + ((start - vma_start) << PAGE_SHIFT);
	size = (end - start) << PAGE_SHIFT;
	zap_special_vma_range(vma, addr, size);
}

static void cla_domain_zap(struct cla_domain *domain, struct cla_ctx *ctx)
{
	struct vm_area_struct *vma;
	struct vma_iterator vmi;

	/*
	 * If the context is dying it has already been unmapped, no need to zap
	 * it here.
	 */
	if (!ctx || cla_ctx_is_dying(ctx))
		return;

	/*
	 * Iterate over vmas in prev_ctx's mm, looking for vmas that map
	 * prev_ctx's file. Zap any portions of those vmas that map the domain.
	 */
	mmap_read_lock(ctx->key.mm);
	vma_iter_init(&vmi, ctx->key.mm, 0);
	for_each_vma(vmi, vma) {
		if (vma->vm_file != ctx->key.file)
			continue;

		cla_vma_zap_domain(vma, domain);
	}
	mmap_read_unlock(ctx->key.mm);
}

static void cla_domain_reassign(struct kthread_work *work)
{
	struct cla_ctx invalid_ctx;
	struct cla_domain *domain;
	struct cla_ctx *prev_ctx;
	struct cla_ctx *next_ctx;
	int ret;

	domain = container_of(work, struct cla_domain, reassign.work);

	mutex_lock(&domain->lock);
	if (domain->broken)
		goto out_unlock;

	/* Figure out prev_ctx and next_ctx. NULL indicates don't [un]assign. */
	next_ctx = cla_domain_get_next_ctx(domain);
	prev_ctx = cla_domain_get_prev_ctx(domain, next_ctx);

	if (!prev_ctx && !next_ctx)
		goto out_unlock;

	/*
	 * We need to grab mmap_lock, but can't do so while holding the domain
	 * lock (since cla_vma_fault() grabs the domain lock while holding
	 * mmap_lock). We are the only thread that modifies assigned_ctx, but
	 * the queue may change and contexts may be killed by cla_ctx_unmap().
	 *
	 * No user is allowed to access CLA while we are reassigning. Disable
	 * assigned_ctx before zapping so cla_vma_fault() won't re-map the
	 * domain during reassignment. A NULL assigned_ctx causes
	 * cla_vma_fault() to immediately schedule a reassignment; use a
	 * reserved context to make it wait.
	 */
	WRITE_ONCE(domain->assigned_ctx, &invalid_ctx);
	mutex_unlock(&domain->lock);

	/* Prevent prev_ctx from accessing any device in the domain. */
	cla_domain_zap(domain, prev_ctx);

	/* Do per-device reassignment work and wait for it to complete. */
	ret = cla_domain_switch_ctx(domain, prev_ctx, next_ctx);
	if (ret)
		next_ctx = NULL;

	mutex_lock(&domain->lock);
	WRITE_ONCE(domain->assigned_ctx, next_ctx);

	/*
	 * Remove next_ctx from the queue and wake up all waiters so that the
	 * fault handler can map the domain into the newly assigned process.
	 * assigned_ctx must have been updated prior to waking the waiters.
	 *
	 * If next_ctx was killed while we were assigning it, insert a
	 * deassignment before the upcoming reclaim.
	 */
	if (next_ctx) {
		if (cla_ctx_is_dying(next_ctx)) {
			cla_domain_schedule_reassignment(domain, 0);
			cla_domain_schedule_reclaim(domain);
			goto out_unlock;
		}
		list_del_init(&next_ctx->queue_node);
		wake_up_all(&next_ctx->waitq);
	}

	/* If there are more queued ctxs, schedule the next reassignment. */
	if (!list_empty(&domain->queued_ctxs))
		cla_domain_schedule_reassignment(domain, CLA_SLICE_MS);

out_unlock:
	mutex_unlock(&domain->lock);
}

static void cla_domain_reclaim(struct kthread_work *work)
{
	struct cla_domain *domain;
	struct cla_ctx *ctx, *next;

	domain = container_of(work, struct cla_domain, reclaim.work);

	mutex_lock(&domain->lock);
	list_for_each_entry_safe(ctx, next, &domain->dying_ctxs, queue_node) {
		list_del_init(&ctx->queue_node);
		WARN_ON(domain->assigned_ctx == ctx);
		cla_ctx_put(ctx);
	}
	mutex_unlock(&domain->lock);
}

/**
 * cla_domain_sched_init - Initialize scheduling for a CLA domain
 * @domain: CLA domain
 *
 * Initialize domain context table, scheduling queues, worker, and work items.
 *
 * Return: 0 on success, or an error
 */
int cla_domain_sched_init(struct cla_domain *domain)
{
	int ret;

	INIT_LIST_HEAD(&domain->queued_ctxs);
	INIT_LIST_HEAD(&domain->dying_ctxs);
	WRITE_ONCE(domain->assigned_ctx, NULL);

	ret = rhashtable_init(&domain->ctxs, &ctxs_rht_params);
	if (ret)
		return ret;

	domain->worker = kthread_run_worker(0, "cla-domain-worker");
	if (IS_ERR(domain->worker)) {
		rhashtable_destroy(&domain->ctxs);
		return PTR_ERR(domain->worker);
	}
	kthread_init_delayed_work(&domain->reassign, cla_domain_reassign);
	kthread_init_delayed_work(&domain->reclaim, cla_domain_reclaim);

	return 0;
}

/**
 * cla_domain_sched_exit - Tear down scheduling for a CLA domain
 * @domain: CLA domain
 *
 * Cancel pending reassignment work, flush reclaim work, destroy the domain
 * worker, and destroy the context table.
 */
void cla_domain_sched_exit(struct cla_domain *domain)
{
	WARN_ON(!list_empty(&domain->queued_ctxs));
	WARN_ON(READ_ONCE(domain->assigned_ctx));

	kthread_cancel_delayed_work_sync(&domain->reassign);
	/* Since reclaim is always queued with 0 delay, it gets flushed here. */
	kthread_destroy_worker(domain->worker);
	rhashtable_destroy(&domain->ctxs);
}

/**
 * cla_domain_set_broken - Mark a CLA domain as broken
 * @domain: CLA domain
 *
 * Prevent further reassignment work and wake contexts waiting for assignment.
 */
void cla_domain_set_broken(struct cla_domain *domain)
{
	struct cla_ctx *ctx;

	mutex_lock(&domain->lock);
	WRITE_ONCE(domain->broken, true);
	/* Notify any waiter that their context isn't getting reassigned */
	list_for_each_entry(ctx, &domain->queued_ctxs, queue_node)
		wake_up_all(&ctx->waitq);
	mutex_unlock(&domain->lock);
}

/**
 * cla_domain_lookup_ctx - Look up a context in a CLA domain
 * @domain: CLA domain
 * @mm: process address space
 * @file: CLA device file
 *
 * The caller must hold the domain lock.
 *
 * Return: the matching context, or %NULL if no context was found
 */
struct cla_ctx *cla_domain_lookup_ctx(struct cla_domain *domain,
				      struct mm_struct *mm, struct file *file)
{
	struct cla_ctx_key key = {
		.mm = mm,
		.file = file,
	};

	lockdep_assert_held(&domain->lock);
	return rhashtable_lookup_fast(&domain->ctxs, &key, ctxs_rht_params);
}

/**
 * cla_domain_insert_ctx - Insert a context into a CLA domain
 * @domain: CLA domain
 * @ctx: context to insert
 *
 * The caller must hold the domain lock.
 *
 * Return: 0 on success, or an error
 */
int cla_domain_insert_ctx(struct cla_domain *domain, struct cla_ctx *ctx)
{
	lockdep_assert_held(&domain->lock);
	return rhashtable_insert_fast(&domain->ctxs, &ctx->node, ctxs_rht_params);
}

/**
 * cla_domain_remove_ctx - Remove a context from a CLA domain
 * @domain: CLA domain
 * @ctx: context to remove
 *
 * The caller must hold the domain lock.
 */
void cla_domain_remove_ctx(struct cla_domain *domain, struct cla_ctx *ctx)
{
	lockdep_assert_held(&domain->lock);
	rhashtable_remove_fast(&domain->ctxs, &ctx->node, ctxs_rht_params);
}

/**
 * cla_domain_schedule_reassignment - Schedule domain reassignment
 * @domain: CLA domain
 * @ms: delay in milliseconds
 *
 * The caller must hold the domain lock. Nop if the domain is broken.
 */
void cla_domain_schedule_reassignment(struct cla_domain *domain, unsigned long ms)
{
	lockdep_assert_held(&domain->lock);
	if (!domain->broken)
		kthread_mod_delayed_work(domain->worker, &domain->reassign,
					 msecs_to_jiffies(ms));
}

/**
 * cla_domain_schedule_reclaim - Schedule reclaim of dying contexts
 * @domain: CLA domain
 *
 * The caller must hold the domain lock. Reclaim is queued without delay after
 * any work already pending on the domain worker.
 */
void cla_domain_schedule_reclaim(struct cla_domain *domain)
{
	lockdep_assert_held(&domain->lock);
	/*
	 * Reclaim is always queued with 0 delay. It is a delayed work only so
	 * we can easily move the work to the back of the queue (after
	 * reassign). Doing so is tricky with non-delayed work.
	 */
	kthread_mod_delayed_work(domain->worker, &domain->reclaim, 0);
}
