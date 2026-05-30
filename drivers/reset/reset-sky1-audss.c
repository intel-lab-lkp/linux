// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 Cix Technology Group Co., Ltd.

#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#include <dt-bindings/reset/sky1-reset-audss.h>

#define SKY1_RESET_SLEEP_MIN_US		50
#define SKY1_RESET_SLEEP_MAX_US		100

struct sky1_audss_signal {
	unsigned int offset;
	unsigned int bit;
};

struct sky1_audss_variant {
	const struct sky1_audss_signal *signals;
	unsigned int signals_num;
};

struct sky1_audss {
	struct reset_controller_dev rcdev;
	const struct sky1_audss_signal *signals;
	struct regmap *regmap;
};

enum {
	AUDSS_SW_RST = 0x78,
};

static const struct sky1_audss_signal sky1_audss_signals[SKY1_AUDSS_SW_RESET_NUM] = {
	[AUDSS_I2S0_SW_RST_N]   = { AUDSS_SW_RST, BIT(0) },
	[AUDSS_I2S1_SW_RST_N]   = { AUDSS_SW_RST, BIT(1) },
	[AUDSS_I2S2_SW_RST_N]   = { AUDSS_SW_RST, BIT(2) },
	[AUDSS_I2S3_SW_RST_N]   = { AUDSS_SW_RST, BIT(3) },
	[AUDSS_I2S4_SW_RST_N]   = { AUDSS_SW_RST, BIT(4) },
	[AUDSS_I2S5_SW_RST_N]   = { AUDSS_SW_RST, BIT(5) },
	[AUDSS_I2S6_SW_RST_N]   = { AUDSS_SW_RST, BIT(6) },
	[AUDSS_I2S7_SW_RST_N]   = { AUDSS_SW_RST, BIT(7) },
	[AUDSS_I2S8_SW_RST_N]   = { AUDSS_SW_RST, BIT(8) },
	[AUDSS_I2S9_SW_RST_N]   = { AUDSS_SW_RST, BIT(9) },
	[AUDSS_WDT_SW_RST_N]    = { AUDSS_SW_RST, BIT(10) },
	[AUDSS_TIMER_SW_RST_N]  = { AUDSS_SW_RST, BIT(11) },
	[AUDSS_MB0_SW_RST_N]    = { AUDSS_SW_RST, BIT(12) },
	[AUDSS_MB1_SW_RST_N]    = { AUDSS_SW_RST, BIT(13) },
	[AUDSS_HDA_SW_RST_N]    = { AUDSS_SW_RST, BIT(14) },
	[AUDSS_DMAC_SW_RST_N]   = { AUDSS_SW_RST, BIT(15) },
};

static const struct sky1_audss_variant variant_sky1_audss = {
	.signals = sky1_audss_signals,
	.signals_num = ARRAY_SIZE(sky1_audss_signals),
};

static struct sky1_audss *to_sky1_audss(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct sky1_audss, rcdev);
}

static int sky1_reset_set(struct reset_controller_dev *rcdev,
			  unsigned long id, bool assert)
{
	struct sky1_audss *sky1rst = to_sky1_audss(rcdev);
	const struct sky1_audss_signal *signal = &sky1rst->signals[id];
	unsigned int value = assert ? 0 : signal->bit;

	return regmap_update_bits(sky1rst->regmap,
				  signal->offset, signal->bit, value);
}

static int sky1_audss_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	sky1_reset_set(rcdev, id, true);
	usleep_range(SKY1_RESET_SLEEP_MIN_US,
		     SKY1_RESET_SLEEP_MAX_US);
	return 0;
}

static int sky1_audss_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	sky1_reset_set(rcdev, id, false);
	usleep_range(SKY1_RESET_SLEEP_MIN_US,
		     SKY1_RESET_SLEEP_MAX_US);
	return 0;
}

static int sky1_audss_reset(struct reset_controller_dev *rcdev,
			    unsigned long id)
{
	sky1_audss_reset_assert(rcdev, id);
	sky1_audss_reset_deassert(rcdev, id);
	return 0;
}

static int sky1_audss_reset_status(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	unsigned int value = 0;
	struct sky1_audss *sky1rst = to_sky1_audss(rcdev);
	const struct sky1_audss_signal *signal = &sky1rst->signals[id];

	regmap_read(sky1rst->regmap, signal->offset, &value);
	return !(value & signal->bit);
}

static const struct reset_control_ops sky1_audss_ops = {
	.reset    = sky1_audss_reset,
	.assert   = sky1_audss_reset_assert,
	.deassert = sky1_audss_reset_deassert,
	.status   = sky1_audss_reset_status
};

static int sky1_audss_reset_probe(struct platform_device *pdev)
{
	const struct sky1_audss_variant *variant;
	struct device *dev = &pdev->dev;
	struct sky1_audss *sky1rst;
	struct device_node *parent_np;
	struct regmap *regmap_cru;

	parent_np = of_get_parent(pdev->dev.of_node);
	regmap_cru = syscon_node_to_regmap(parent_np);
	of_node_put(parent_np);
	if (IS_ERR(regmap_cru))
		return dev_err_probe(dev, PTR_ERR(regmap_cru),
				     "unable to get audss_cru regmap");

	sky1rst = devm_kzalloc(dev, sizeof(*sky1rst), GFP_KERNEL);
	if (!sky1rst)
		return -ENOMEM;

	variant = of_device_get_match_data(dev);
	if (!variant)
		return -ENODEV;

	sky1rst->regmap          = regmap_cru;
	sky1rst->signals         = variant->signals;
	sky1rst->rcdev.owner     = THIS_MODULE;
	sky1rst->rcdev.nr_resets = variant->signals_num;
	sky1rst->rcdev.ops       = &sky1_audss_ops;
	sky1rst->rcdev.of_node   = dev->of_node;
	sky1rst->rcdev.dev       = dev;

	return devm_reset_controller_register(dev, &sky1rst->rcdev);
}

static const struct of_device_id sky1_audss_reset_of_match[] = {
	{ .compatible = "cix,sky1-audss-reset", .data = &variant_sky1_audss},
	{},
};
MODULE_DEVICE_TABLE(of, sky1_audss_reset_of_match);

static struct platform_driver sky1_audss_reset_driver = {
	.probe	= sky1_audss_reset_probe,
	.driver = {
		.name		= "sky1-audss-rst",
		.of_match_table = sky1_audss_reset_of_match,
	},
};
module_platform_driver(sky1_audss_reset_driver)

MODULE_AUTHOR("Joakim Zhang <joakim.zhang@cixtech.com>");
MODULE_DESCRIPTION("Cix Sky1 audss reset driver");
MODULE_LICENSE("GPL");
