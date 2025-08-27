/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Samsung Exynos Alive Clock and Power Manager (ACPM) clock driver.
 *
 * Copyright 2025 Linaro Ltd.
 */

#ifndef __LINUX_PLATFORM_DATA_CLK_ACPM_H__
#define __LINUX_PLATFORM_DATA_CLK_ACPM_H__

#include <linux/types.h>

struct acpm_clk_variant {
	unsigned int id;
	const char *name;
};

struct acpm_clk_platform_data {
	const struct acpm_clk_variant *clks;
	unsigned int nr_clks;
	unsigned int mbox_chan_id;
};

#endif /* __LINUX_PLATFORM_DATA_CLK_ACPM_H__ */
