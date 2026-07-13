// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define LPASS_RATE_GEN_CTRL		0xD000
#define LPASS_RATE_GEN_COUNTER_0	0xD004
#define LPASS_RATE_GEN_DELAY		0xD010

#define LPASS_RATE_GEN_MAX_REG		LPASS_RATE_GEN_DELAY

#define LPASS_RG_CTRL_EN		BIT(0)

struct lpass_va_csr_data {
	u32 counter_0;
	u32 delay;
};

static const struct lpass_va_csr_data hawi_csr_data = {
	.counter_0 = 0x960,
	.delay = 0x16,
};

static const struct regmap_config lpass_rate_gen_regmap_config = {
	.name = "lpass_rate_gen",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = LPASS_RATE_GEN_MAX_REG,
	.cache_type = REGCACHE_MAPLE,
};

struct lpass_va_csr {
	struct regmap *regmap;
	const struct lpass_va_csr_data *data;
	struct clk_hw hb_hw;
};

#define to_lpass_va_csr(_hw) container_of(_hw, struct lpass_va_csr, hb_hw)

static int heartbeat_pulse_enable(struct clk_hw *hw)
{
	struct lpass_va_csr *csr = to_lpass_va_csr(hw);

	regmap_write(csr->regmap, LPASS_RATE_GEN_COUNTER_0, csr->data->counter_0);
	regmap_write(csr->regmap, LPASS_RATE_GEN_DELAY, csr->data->delay);
	regmap_update_bits(csr->regmap, LPASS_RATE_GEN_CTRL,
			   LPASS_RG_CTRL_EN, LPASS_RG_CTRL_EN);

	return 0;
}

static void heartbeat_pulse_disable(struct clk_hw *hw)
{
	struct lpass_va_csr *csr = to_lpass_va_csr(hw);

	regmap_update_bits(csr->regmap, LPASS_RATE_GEN_CTRL,
			   LPASS_RG_CTRL_EN, 0);
}

static int heartbeat_pulse_is_enabled(struct clk_hw *hw)
{
	struct lpass_va_csr *csr = to_lpass_va_csr(hw);
	unsigned int val;

	regmap_read(csr->regmap, LPASS_RATE_GEN_CTRL, &val);

	return !!(val & LPASS_RG_CTRL_EN);
}

static const struct clk_ops heartbeat_pulse_ops = {
	.enable = heartbeat_pulse_enable,
	.disable = heartbeat_pulse_disable,
	.is_enabled = heartbeat_pulse_is_enabled,
};

static int lpass_va_csr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lpass_va_csr *csr;
	struct clk_init_data init = {
		.name = "lpass_heartbeat_pulse",
		.ops = &heartbeat_pulse_ops,
	};
	void __iomem *base;
	int ret;

	csr = devm_kzalloc(dev, sizeof(*csr), GFP_KERNEL);
	if (!csr)
		return -ENOMEM;

	csr->data = of_device_get_match_data(dev);
	if (!csr->data)
		return dev_err_probe(dev, -EINVAL, "no variant data for compatible\n");

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	csr->regmap = devm_regmap_init_mmio(dev, base,
					    &lpass_rate_gen_regmap_config);
	if (IS_ERR(csr->regmap))
		return dev_err_probe(dev, PTR_ERR(csr->regmap),
				     "failed to init regmap\n");

	csr->hb_hw.init = &init;

	ret = devm_clk_hw_register(dev, &csr->hb_hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register heartbeat clock\n");

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &csr->hb_hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock provider\n");

	return 0;
}

static const struct of_device_id lpass_va_csr_dt_match[] = {
	{ .compatible = "qcom,hawi-lpass-va-csr", .data = &hawi_csr_data },
	{}
};
MODULE_DEVICE_TABLE(of, lpass_va_csr_dt_match);

static struct platform_driver lpass_va_csr_driver = {
	.driver = {
		.name = "qcom-lpass-va-csr",
		.of_match_table = lpass_va_csr_dt_match,
	},
	.probe = lpass_va_csr_probe,
};

module_platform_driver(lpass_va_csr_driver);

MODULE_DESCRIPTION("Qualcomm LPASS VA CSR heartbeat pulse clock provider");
MODULE_LICENSE("GPL");
