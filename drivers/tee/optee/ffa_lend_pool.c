// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for the arm,ffa-lend-pool, unmaps lent memory from the host stage-1
 * to mitigate for CPU speculative read of Secure memory
 *
 * Copyright (C) 2026 Google LLC.
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <linux/arm_ffa.h>
#include <linux/errno.h>
#include <linux/of_reserved_mem.h>

#include "optee_private.h"

static struct device_node *optee_dev_node(struct optee *optee)
{
	return dev_of_node(optee->teedev->dev.parent);
}

static struct device *optee_device(struct optee *optee)
{
	return &optee->teedev->dev;
}

static int optee_lend_pool_err(int err)
{
	/*
	 * Registration of the arm,ffa-lend-pool reserved-memory is optional as
	 * another (although less performant) stage-2 mitigation might be in
	 * place.
	 */
	if (err == -ENODEV)
		return 0;

	return err;
}

int optee_lend_pool_register(struct optee *optee)
{
	struct device_node *np = optee_dev_node(optee);
	struct device *dev = optee_device(optee);
	int ret;

	if (np)
		ret = of_reserved_mem_device_init_by_idx(dev, np, 0);
	else
		ret = ffa_lend_pool_attach(dev);

	return optee_lend_pool_err(ret);
}

void optee_lend_pool_unregister(struct optee *optee)
{
	struct device_node *np = optee_dev_node(optee);
	struct device *dev = optee_device(optee);

	if (np)
		of_reserved_mem_device_release(dev);
	else
		ffa_lend_pool_detach(dev);
}

int optee_lend_pool_prepare(struct optee *optee, struct page *page, u64 nr_pages)
{
	return optee_lend_pool_err(ffa_prepare_lend(optee_device(optee), page, nr_pages));
}

void optee_lend_pool_reclaimed(struct optee *optee, struct page *page, u64 nr_pages)
{
	ffa_lend_reclaimed(optee_device(optee), page, nr_pages);
}
