// SPDX-License-Identifier: GPL-2.0
/*
 * Common Primitives for DAMON Modules
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/damon.h>

#include "modules-common.h"

static int __damon_modules_new_paddr_kdamond(struct kdamond *kdamond)
{
	struct damon_ctx *ctx;
	struct damon_target *target;

	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;

	if (damon_select_ops(ctx, DAMON_OPS_PADDR)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}

	target = damon_new_target();
	if (!target) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}

	damon_add_target(ctx, target);
	damon_add_ctx(kdamond, ctx);

	return 0;
}

/*
 * Allocate, set, and return a DAMON daemon for the physical address space.
 * @kdamondp:	Pointer to save the point to the newly created kdamond
 */
int damon_modules_new_paddr_kdamond(struct kdamond **kdamondp)
{
	int err;
	struct kdamond *kdamond;

	kdamond = damon_new_kdamond();
	if (!kdamond)
		return -ENOMEM;

	err = __damon_modules_new_paddr_kdamond(kdamond);
	if (err) {
		damon_destroy_kdamond(kdamond);
		return err;
	}

	*kdamondp = kdamond;
	return 0;
}
