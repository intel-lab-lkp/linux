// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 NVIDIA Corporation & Affiliates */
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "cxl.h"
#include "core.h"

struct cxl_rwsem cxl_rwsem = {
	.region = __RWSEM_INITIALIZER(cxl_rwsem.region),
	.dpa = __RWSEM_INITIALIZER(cxl_rwsem.dpa),
};

static void cxld_set_interleave(struct cxl_decoder *cxld, u32 *ctrl)
{
	u16 eig;
	u8 eiw;

	/*
	 * Input validation ensures these warns never fire, but otherwise
	 * suppress unititalized variable usage warnings.
	 */
	if (WARN_ONCE(ways_to_eiw(cxld->interleave_ways, &eiw),
		      "invalid interleave_ways: %d\n", cxld->interleave_ways))
		return;
	if (WARN_ONCE(granularity_to_eig(cxld->interleave_granularity, &eig),
		      "invalid interleave_granularity: %d\n",
		      cxld->interleave_granularity))
		return;

	u32p_replace_bits(ctrl, eig, CXL_HDM_DECODER0_CTRL_IG_MASK);
	u32p_replace_bits(ctrl, eiw, CXL_HDM_DECODER0_CTRL_IW_MASK);
	*ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
}

static void cxld_set_type(struct cxl_decoder *cxld, u32 *ctrl)
{
	u32p_replace_bits(ctrl,
			  !!(cxld->target_type == CXL_DECODER_HOSTONLYMEM),
			  CXL_HDM_DECODER0_CTRL_HOSTONLY);
}

/*
 * Per CXL 2.0 8.2.5.12.20 Committing Decoder Programming, hardware must set
 * committed or error within 10ms, but just be generous with 20ms to account for
 * clock skew and other marginal behavior
 */
#define COMMIT_TIMEOUT_MS 20
int cxld_await_commit(void __iomem *hdm, int id)
{
	u32 ctrl;
	int i;

	for (i = 0; i < COMMIT_TIMEOUT_MS; i++) {
		ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMIT_ERROR, ctrl)) {
			ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMIT;
			writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
			return -EIO;
		}
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
			return 0;
		fsleep(1000);
	}

	return -ETIMEDOUT;
}

void setup_hw_decoder(struct cxl_decoder *cxld, void __iomem *hdm)
{
	int id = cxld->id;
	u64 base, size;
	u32 ctrl;

	/* common decoder settings */
	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(cxld->id));
	cxld_set_interleave(cxld, &ctrl);
	cxld_set_type(cxld, &ctrl);
	base = cxld->hpa_range.start;
	size = range_len(&cxld->hpa_range);

	writel(upper_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
	writel(lower_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	writel(upper_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
	writel(lower_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));

	if (is_switch_decoder(&cxld->dev)) {
		struct cxl_switch_decoder *cxlsd =
			to_cxl_switch_decoder(&cxld->dev);
		void __iomem *tl_hi = hdm + CXL_HDM_DECODER0_TL_HIGH(id);
		void __iomem *tl_lo = hdm + CXL_HDM_DECODER0_TL_LOW(id);
		u64 targets;

		cxlsd_set_targets(cxlsd, &targets);
		writel(upper_32_bits(targets), tl_hi);
		writel(lower_32_bits(targets), tl_lo);
	} else {
		struct cxl_endpoint_decoder *cxled =
			to_cxl_endpoint_decoder(&cxld->dev);
		void __iomem *sk_hi = hdm + CXL_HDM_DECODER0_SKIP_HIGH(id);
		void __iomem *sk_lo = hdm + CXL_HDM_DECODER0_SKIP_LOW(id);

		writel(upper_32_bits(cxled->skip), sk_hi);
		writel(lower_32_bits(cxled->skip), sk_lo);
	}

	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
}
