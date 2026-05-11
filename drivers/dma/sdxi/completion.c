// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI Descriptor Completion Status Block handling.
 *
 * Copyright Advanced Micro Devices, Inc.
 */
#include <linux/cleanup.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/jiffies.h>
#include <linux/slab.h>

#include "completion.h"
#include "descriptor.h"
#include "hw.h"

struct sdxi_completion {
	struct sdxi_dev *sdxi;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
};

struct sdxi_completion *sdxi_completion_alloc(struct sdxi_dev *sdxi)
{
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;

	/*
	 * Assume callers can't tolerate GFP_KERNEL and use
	 * GFP_NOWAIT. Add a gfp_t flags parameter if that changes.
	 */
	struct sdxi_completion *sc __free(kfree) = kmalloc(sizeof(*sc), GFP_NOWAIT);
	if (!sc)
		return NULL;

	cst_blk = dma_pool_zalloc(sdxi->cst_blk_pool, GFP_NOWAIT, &cst_blk_dma);
	if (!cst_blk)
		return NULL;

	cst_blk->signal = cpu_to_le64(1);

	*sc = (typeof(*sc)) {
		.sdxi        = sdxi,
		.cst_blk     = cst_blk,
		.cst_blk_dma = cst_blk_dma,
	};

	return_ptr(sc);
}

void sdxi_completion_free(struct sdxi_completion *sc)
{
	dma_pool_free(sc->sdxi->cst_blk_pool, sc->cst_blk, sc->cst_blk_dma);
	kfree(sc);
}

int sdxi_completion_poll(const struct sdxi_completion *sc)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);

	while (le64_to_cpu(READ_ONCE(sc->cst_blk->signal)) != 0) {
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		cpu_relax();
	}

	return sdxi_completion_errored(sc) ? -EIO : 0;
}

bool sdxi_completion_signaled(const struct sdxi_completion *sc)
{
	dma_rmb();
	return (sc->cst_blk->signal == 0);
}

bool sdxi_completion_errored(const struct sdxi_completion *sc)
{
	dma_rmb();
	return FIELD_GET(SDXI_CST_BLK_ER_BIT, le32_to_cpu(sc->cst_blk->flags));
}


void sdxi_completion_attach(struct sdxi_desc *desc,
			    const struct sdxi_completion *cs)
{
	sdxi_desc_set_csb(desc, cs->cst_blk_dma);
}
