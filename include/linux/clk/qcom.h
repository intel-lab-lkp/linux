/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __LINUX_CLK_QCOM_H
#define __LINUX_CLK_QCOM_H

#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct device;
struct platform_device;
struct regulator_bulk_data;

/**
 * struct qcom_clk_ref_desc - description of a simple XO reference gate
 * @name: clock name
 * @offset: register offset
 * @regulator_names: optional per-clock regulator names
 * @num_regulators: number of entries in @regulator_names
 */
struct qcom_clk_ref_desc {
	const char *name;
	u32 offset;
	const char * const *regulator_names;
	unsigned int num_regulators;
};

/**
 * struct qcom_clk_ref - runtime state for a qcom reference gate
 * @hw: clock hardware handle
 * @init_data: clock init data
 * @regmap: register map backing the gate register
 * @desc: clock descriptor copied at registration
 * @regulators: optional regulator handles
 */
struct qcom_clk_ref {
	struct clk_hw hw;
	struct clk_init_data init_data;
	struct regmap *regmap;
	struct qcom_clk_ref_desc desc;
	struct regulator_bulk_data *regulators;
};

#if IS_ENABLED(CONFIG_COMMON_CLK_QCOM)

int qcom_clk_ref_probe(struct platform_device *pdev,
		       const struct regmap_config *config,
		       const struct qcom_clk_ref_desc *descs,
		       size_t num_clk_refs);

#else

static inline int
qcom_clk_ref_probe(struct platform_device *pdev,
		   const struct regmap_config *config,
		   const struct qcom_clk_ref_desc *descs,
		   size_t num_clk_refs)
{
	return -EOPNOTSUPP;
}

#endif

#endif
