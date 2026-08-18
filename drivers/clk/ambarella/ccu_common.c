// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ambarella, Inc.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "ccu_common.h"

static const struct regmap_config amb_rct_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = AMB_RCT_REG_SIZE - 4,
};

struct amb_ccu *amb_ccu_init(struct platform_device *pdev,
			     unsigned int num_clks)
{
	struct amb_ccu *ccu;
	void __iomem *base;

	if (!num_clks)
		return ERR_PTR(-EINVAL);

	ccu = devm_kzalloc(&pdev->dev, sizeof(*ccu), GFP_KERNEL);
	if (!ccu)
		return ERR_PTR(-ENOMEM);

	ccu->dev = &pdev->dev;

	ccu->data = devm_kzalloc(&pdev->dev,
				 struct_size(ccu->data, hws, num_clks),
				 GFP_KERNEL);
	if (!ccu->data)
		return ERR_PTR(-ENOMEM);

	ccu->data->num = num_clks;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return ERR_CAST(base);

	ccu->map = devm_regmap_init_mmio(&pdev->dev, base,
					 &amb_rct_regmap_config);
	if (IS_ERR(ccu->map))
		return ERR_CAST(ccu->map);

	return ccu;
}

int amb_ccu_register(struct amb_ccu *ccu)
{
	return devm_of_clk_add_hw_provider(ccu->dev, of_clk_hw_onecell_get,
					   ccu->data);
}
