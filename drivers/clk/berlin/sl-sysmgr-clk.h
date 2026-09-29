/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 *
 */

#ifndef __SL_SYSMGR_CLK_H
#define __SL_SYSMGR_CLK_H

struct sl_sysmgr_clk_desc {
	const char	*name;
	const char	*parent_name;
	u32		id;
	u32		offset;
	u8		div_bits;
	unsigned long	flags;
};

struct sl_sysmgr_gateclk_desc {
	const char	*name;
	const char	*parent_name;
	u32		id;
	u8		bit_idx;
	unsigned long	flags;
};

struct sl_sysmgr_fixedclk_desc {
	const char	*name;
	u32		id;
	unsigned long	rate;
};

int sl_sysmgr_clk_setup(struct platform_device *pdev,
			const struct sl_sysmgr_fixedclk_desc *fdescs,
			int fclk_n,
			const struct sl_sysmgr_gateclk_desc *gdescs,
			int gclk_n,
			u32 gclk_offset,
			const struct sl_sysmgr_clk_desc *descs,
			int clk_n);

#endif
