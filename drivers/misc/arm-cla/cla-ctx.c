// SPDX-License-Identifier: GPL-2.0

#include <linux/err.h>
#include <linux/file.h>
#include <linux/gfp.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "arm-cla.h"

/**
 * cla_ctx_map - Map a process and file to a CLA domain context
 * @domain: CLA domain
 * @mm: process address space
 * @file: CLA device file
 *
 * Reuse an existing context for the same @mm and @file, or allocate and insert
 * a new context into @domain. The context mapping count is incremented before
 * returning.
 *
 * Return: the mapped context on success, or an error pointer on failure
 */
struct cla_ctx *cla_ctx_map(struct cla_domain *domain, struct mm_struct *mm,
			    struct file *file)
{
	struct cla_ctx *ctx;
	int ret = -ENOMEM;

	mutex_lock(&domain->lock);
	if (domain->broken) {
		ret = -EIO;
		goto err_unlock;
	}

	ctx = cla_domain_lookup_ctx(domain, mm, file);
	if (ctx) {
		refcount_inc(&ctx->mapped);
		mutex_unlock(&domain->lock);
		return ctx;
	}

	ctx = kzalloc_obj(*ctx, GFP_KERNEL_ACCOUNT);
	if (!ctx)
		goto err_unlock;

	kref_init(&ctx->refcnt);
	refcount_set(&ctx->mapped, 1);
	ctx->domain = domain;
	ctx->regs = cla_regs_alloc_domain(domain);
	if (!ctx->regs)
		goto err_free;

	mmgrab(mm);
	ctx->key.mm = mm;
	get_file(file);
	ctx->key.file = file;
	INIT_LIST_HEAD(&ctx->queue_node);
	init_waitqueue_head(&ctx->waitq);

	ret = cla_domain_insert_ctx(domain, ctx);
	if (ret)
		goto err_mmdrop;
	mutex_unlock(&domain->lock);

	return ctx;

err_mmdrop:
	mmdrop(ctx->key.mm);
	fput(ctx->key.file);
	cla_regs_free_domain(domain, ctx->regs);
err_free:
	kfree(ctx);
err_unlock:
	mutex_unlock(&domain->lock);
	return ERR_PTR(ret);
}

/**
 * cla_ctx_unmap - Unmap a process and file from a CLA domain context
 * @domain: CLA domain
 * @mm: process address space
 * @file: CLA device file
 *
 * Drop a context mapping. When the final mapping is removed, wake waiters,
 * remove the context from @domain, and schedule it for deassignment and
 * reclaim.
 */
void cla_ctx_unmap(struct cla_domain *domain, struct mm_struct *mm,
		   struct file *file)
{
	struct cla_ctx *ctx;

	mutex_lock(&domain->lock);
	ctx = cla_domain_lookup_ctx(domain, mm, file);
	WARN_ON(!ctx);

	if (!refcount_dec_and_test(&ctx->mapped)) {
		mutex_unlock(&domain->lock);
		return;
	}

	/* Notify waiters that their context is dying. */
	wake_up_all(&ctx->waitq);
	cla_domain_remove_ctx(domain, ctx);

	/*
	 * Since we're holding the mmap_lock for writing, and reassignment work
	 * may be waiting to grab it for reading, we cannot wait for
	 * reassignment here. The context holds a reference to mm_count, so the
	 * mm_struct or pgd are not going away until cla_ctx_free().
	 * If the mm is exiting, the accelerators will now access memory through
	 * an empty pgd and fault silently.
	 *
	 * Schedule deassignment followed by reclaim. If reassignment is
	 * currently running, it reschedules itself once it re-takes the lock:
	 * - After deassigning this context, schedule reassignment as usual. If
	 *   no more contexts are queued, the following reassignment is a NOP.
	 * - After assigning this context, insert a deassignment before reclaim.
	 */
	if (domain->assigned_ctx == ctx)
		cla_domain_schedule_reassignment(domain, 0);
	list_move(&ctx->queue_node, &domain->dying_ctxs);
	cla_domain_schedule_reclaim(domain);
	mutex_unlock(&domain->lock);
}

/**
 * cla_ctx_free - Free a CLA domain context
 * @ref: context reference counter
 *
 * Release resources held by a dying context after final reference is dropped.
 */
void cla_ctx_free(struct kref *ref)
{
	struct cla_ctx *ctx = container_of(ref, struct cla_ctx, refcnt);

	WARN_ON(!cla_ctx_is_dying(ctx));
	mmdrop(ctx->key.mm);
	fput(ctx->key.file);
	cla_regs_free_domain(ctx->domain, ctx->regs);
	kfree(ctx);
}
