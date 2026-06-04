// SPDX-License-Identifier: GPL-2.0
/*
 * Common Code for DAMON Modules
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/damon.h>

#include "modules-common.h"

/*
 * Allocate, set, and return a DAMON context.
 * @ctxp:	Pointer to save the point to the newly created context
 * @targetp:	Pointer to save the point to the newly created target
 * id:	DAMOS op id. It can be VADDR or PADDR
 */
static int _damon_modules_new_ctx_target(struct damon_ctx **ctxp,
				struct damon_target **targetp, enum damon_ops_id id)
{
	struct damon_ctx *ctx;
	struct damon_target *target;

	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;

	if (damon_select_ops(ctx, id)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}

	target = damon_new_target();
	if (!target) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_add_target(ctx, target);

	*ctxp = ctx;
	*targetp = target;
	return 0;
}

/*
 * Allocate, set, and return a DAMON context for the physical address space.
 * @ctxp:       Pointer to save the point to the newly created context
 * @targetp:    Pointer to save the point to the newly created target
 */
int damon_modules_new_paddr_ctx_target(struct damon_ctx **ctxp,
				struct damon_target **targetp)
{
	return _damon_modules_new_ctx_target(ctxp, targetp, DAMON_OPS_PADDR);
}

/*
 * Allocate, set, and return a DAMON context for the virtual address space.
 * @ctxp:	Pointer to save the point to the newly created context
 * @targetp:	Pointer to save the point to the newly created target
 */
int damon_modules_new_vaddr_ctx_target(struct damon_ctx **ctxp,
				struct damon_target **targetp)
{
	return _damon_modules_new_ctx_target(ctxp, targetp, DAMON_OPS_VADDR);
}
