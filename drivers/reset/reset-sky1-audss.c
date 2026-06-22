// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cix Sky1 Audio Subsystem reset controller driver
 *
 * Copyright 2026 Cix Technology Group Co., Ltd.
 */

#include <dt-bindings/reset/cix,sky1-audss-reset.h>

#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define SKY1_RESET_SLEEP_MIN_US		50
#define SKY1_RESET_SLEEP_MAX_US		100

#define AUDSS_SW_RST			0x78

struct sky1_audss_reset_map {
	unsigned int offset;
	unsigned int mask;
};

struct sky1_audss_reset {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	const struct sky1_audss_reset_map *map;
};

static const struct sky1_audss_reset_map sky1_audss_reset_map[] = {
	[AUDSS_I2S0_SW_RST]   = { AUDSS_SW_RST, BIT(0) },
	[AUDSS_I2S1_SW_RST]   = { AUDSS_SW_RST, BIT(1) },
	[AUDSS_I2S2_SW_RST]   = { AUDSS_SW_RST, BIT(2) },
	[AUDSS_I2S3_SW_RST]   = { AUDSS_SW_RST, BIT(3) },
	[AUDSS_I2S4_SW_RST]   = { AUDSS_SW_RST, BIT(4) },
	[AUDSS_I2S5_SW_RST]   = { AUDSS_SW_RST, BIT(5) },
	[AUDSS_I2S6_SW_RST]   = { AUDSS_SW_RST, BIT(6) },
	[AUDSS_I2S7_SW_RST]   = { AUDSS_SW_RST, BIT(7) },
	[AUDSS_I2S8_SW_RST]   = { AUDSS_SW_RST, BIT(8) },
	[AUDSS_I2S9_SW_RST]   = { AUDSS_SW_RST, BIT(9) },
	[AUDSS_WDT_SW_RST]    = { AUDSS_SW_RST, BIT(10) },
	[AUDSS_TIMER_SW_RST]  = { AUDSS_SW_RST, BIT(11) },
	[AUDSS_MB0_SW_RST]    = { AUDSS_SW_RST, BIT(12) },
	[AUDSS_MB1_SW_RST]    = { AUDSS_SW_RST, BIT(13) },
	[AUDSS_HDA_SW_RST]    = { AUDSS_SW_RST, BIT(14) },
	[AUDSS_DMAC_SW_RST]   = { AUDSS_SW_RST, BIT(15) },
};

static struct sky1_audss_reset *to_sky1_audss_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct sky1_audss_reset, rcdev);
}

static int sky1_audss_reset_set(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct sky1_audss_reset *priv = to_sky1_audss_reset(rcdev);
	const struct sky1_audss_reset_map *signal = &priv->map[id];
	unsigned int value = assert ? 0 : signal->mask;

	return regmap_update_bits(priv->regmap, signal->offset, signal->mask, value);
}

static int sky1_audss_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	sky1_audss_reset_set(rcdev, id, true);
	usleep_range(SKY1_RESET_SLEEP_MIN_US, SKY1_RESET_SLEEP_MAX_US);
	return 0;
}

static int sky1_audss_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	sky1_audss_reset_set(rcdev, id, false);
	usleep_range(SKY1_RESET_SLEEP_MIN_US, SKY1_RESET_SLEEP_MAX_US);
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
	struct sky1_audss_reset *priv = to_sky1_audss_reset(rcdev);
	const struct sky1_audss_reset_map *signal = &priv->map[id];
	unsigned int value;

	regmap_read(priv->regmap, signal->offset, &value);
	return !!(value & signal->mask);
}

static const struct reset_control_ops sky1_audss_reset_ops = {
	.reset    = sky1_audss_reset,
	.assert   = sky1_audss_reset_assert,
	.deassert = sky1_audss_reset_deassert,
	.status   = sky1_audss_reset_status,
};

static const struct regmap_config sky1_audss_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static void sky1_audss_reset_iounmap(void *data)
{
	iounmap(data);
}

static int sky1_audss_reset_get_regmap(struct sky1_audss_reset *priv)
{
	struct device *dev = priv->rcdev.dev;
	void __iomem *base;
	int ret;

	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (priv->regmap)
		return 0;

	base = of_iomap(dev->parent->of_node, 0);
	if (!base)
		return dev_err_probe(dev, -ENOMEM, "failed to iomap address space\n");

	ret = devm_add_action_or_reset(dev, sky1_audss_reset_iounmap, base);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register iounmap action\n");

	priv->regmap = devm_regmap_init_mmio(dev, base, &sky1_audss_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to initialize regmap\n");

	return 0;
}

static int sky1_audss_reset_probe(struct auxiliary_device *adev,
				  const struct auxiliary_device_id *id)
{
	struct sky1_audss_reset *priv;
	struct device *dev = &adev->dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->map = sky1_audss_reset_map;
	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.nr_resets = ARRAY_SIZE(sky1_audss_reset_map);
	priv->rcdev.ops = &sky1_audss_reset_ops;
	priv->rcdev.of_node = dev->parent->of_node;
	priv->rcdev.dev = dev;
	priv->rcdev.of_reset_n_cells = 1;

	dev_set_drvdata(dev, priv);

	ret = sky1_audss_reset_get_regmap(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regmap\n");

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct auxiliary_device_id sky1_audss_reset_ids[] = {
	{ .name = "clk_sky1_audss.reset" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, sky1_audss_reset_ids);

static struct auxiliary_driver sky1_audss_reset_driver = {
	.probe = sky1_audss_reset_probe,
	.id_table = sky1_audss_reset_ids,
};

module_auxiliary_driver(sky1_audss_reset_driver);

MODULE_AUTHOR("Joakim Zhang <joakim.zhang@cixtech.com>");
MODULE_DESCRIPTION("Cix Sky1 Audio Subsystem reset driver");
MODULE_LICENSE("GPL");
