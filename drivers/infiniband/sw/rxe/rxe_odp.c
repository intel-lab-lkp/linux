// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2022-2023 Fujitsu Ltd. All rights reserved.
 */

#include <linux/hmm.h>

#include <rdma/ib_umem_odp.h>

#include "rxe.h"

static void rxe_mr_unset_xarray(struct rxe_mr *mr, unsigned long start,
				unsigned long end)
{
	unsigned long upper = rxe_mr_iova_to_index(mr, end - 1);
	unsigned long lower = rxe_mr_iova_to_index(mr, start);
	void *entry;

	XA_STATE(xas, &mr->page_list, lower);

	/* make elements in xarray NULL */
	xas_lock(&xas);
	xas_for_each(&xas, entry, upper)
		xas_store(&xas, NULL);
	xas_unlock(&xas);
}

static bool rxe_ib_invalidate_range(struct mmu_interval_notifier *mni,
				    const struct mmu_notifier_range *range,
				    unsigned long cur_seq)
{
	struct ib_umem_odp *umem_odp =
		container_of(mni, struct ib_umem_odp, notifier);
	struct rxe_mr *mr = umem_odp->private;
	unsigned long start, end;

	if (!mmu_notifier_range_blockable(range))
		return false;

	mutex_lock(&umem_odp->umem_mutex);
	mmu_interval_set_seq(mni, cur_seq);

	start = max_t(u64, ib_umem_start(umem_odp), range->start);
	end = min_t(u64, ib_umem_end(umem_odp), range->end);

	rxe_mr_unset_xarray(mr, start, end);

	/* update umem_odp->dma_list */
	ib_umem_odp_unmap_dma_pages(umem_odp, start, end);

	mutex_unlock(&umem_odp->umem_mutex);
	return true;
}

const struct mmu_interval_notifier_ops rxe_mn_ops = {
	.invalidate = rxe_ib_invalidate_range,
};
