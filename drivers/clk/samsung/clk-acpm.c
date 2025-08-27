// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos ACPM protocol based clock driver.
 *
 * Copyright 2025 Linaro Ltd.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/clk-acpm.h>
#include <linux/platform_device.h>
#include <linux/types.h>

struct acpm_clk {
	u32 id;
	struct clk_hw hw;
	unsigned int mbox_chan_id;
	const struct acpm_handle *handle;
};

#define to_acpm_clk(clk) container_of(clk, struct acpm_clk, hw)

static unsigned long acpm_clk_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct acpm_clk *clk = to_acpm_clk(hw);

	return clk->handle->ops.dvfs_ops.get_rate(clk->handle,
					clk->mbox_chan_id, clk->id, 0);
}

static int acpm_clk_determine_rate(struct clk_hw *hw,
				   struct clk_rate_request *req)
{
	/*
	 * We can't figure out what rate it will be, so just return the
	 * rate back to the caller. acpm_clk_recalc_rate() will be called
	 * after the rate is set and we'll know what rate the clock is
	 * running at then.
	 */
	return 0;
}

static int acpm_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct acpm_clk *clk = to_acpm_clk(hw);

	return clk->handle->ops.dvfs_ops.set_rate(clk->handle,
					clk->mbox_chan_id, clk->id, rate);
}

static const struct clk_ops acpm_clk_ops = {
	.recalc_rate = acpm_clk_recalc_rate,
	.determine_rate = acpm_clk_determine_rate,
	.set_rate = acpm_clk_set_rate,
};

static int acpm_clk_ops_init(struct device *dev, struct acpm_clk *aclk,
			     const char *name)
{
	struct clk_init_data init = {};

	init.name = name;
	init.ops = &acpm_clk_ops;
	aclk->hw.init = &init;

	return devm_clk_hw_register(dev, &aclk->hw);
}

static int acpm_clk_probe(struct platform_device *pdev)
{
	const struct acpm_clk_platform_data *pdata;
	const struct acpm_handle *acpm_handle;
	struct clk_hw_onecell_data *clk_data;
	struct clk_hw **hws;
	struct device *dev = &pdev->dev;
	struct acpm_clk *aclks;
	unsigned int mbox_chan_id;
	int i, err, count;

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata)
		return dev_err_probe(dev, -EINVAL,
				     "Failed to get platform data.\n");

	acpm_handle = devm_acpm_get_by_node(dev, dev->parent->of_node);
	if (IS_ERR(acpm_handle))
		return dev_err_probe(dev, PTR_ERR(acpm_handle),
				     "Failed to get acpm handle.\n");

	count = pdata->nr_clks;
	mbox_chan_id = pdata->mbox_chan_id;

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, count),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = count;
	hws = clk_data->hws;

	aclks = devm_kcalloc(dev, count, sizeof(*aclks), GFP_KERNEL);
	if (!aclks)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		const struct acpm_clk_variant *variant = &pdata->clks[i];
		unsigned int id = variant->id;
		struct acpm_clk *aclk;

		if (id >= count)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid ACPM clock ID.\n");

		aclk = &aclks[id];
		aclk->id = id;
		aclk->handle = acpm_handle;
		aclk->mbox_chan_id = mbox_chan_id;

		hws[id] = &aclk->hw;

		err = acpm_clk_ops_init(dev, aclk, variant->name);
		if (err)
			return dev_err_probe(dev, err,
					     "Failed to register clock.\n");
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   clk_data);
}

static struct platform_driver acpm_clk_driver = {
	.driver	= {
		.name = "acpm-clocks",
	},
	.probe = acpm_clk_probe,
};
module_platform_driver(acpm_clk_driver);

MODULE_ALIAS("platform:acpm-clocks");
MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Samsung Exynos ACPM clock driver");
MODULE_LICENSE("GPL");
