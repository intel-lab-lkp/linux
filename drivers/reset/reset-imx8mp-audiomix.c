// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

#define EARC			0x200
#define EARC_RESET_MASK		0x3

struct imx8mp_audiomix_reset_priv {
	struct reset_controller_dev rcdev;
	void __iomem *base;
};

static int imx8mp_audiomix_reset_assert(struct reset_controller_dev *rcdev,
					unsigned long id)
{
	struct imx8mp_audiomix_reset_priv *priv = container_of(rcdev,
					struct imx8mp_audiomix_reset_priv, rcdev);
	void __iomem *reg_addr = priv->base;
	unsigned int mask, reg;

	if (id >= fls(EARC_RESET_MASK))
		return -EINVAL;

	mask = BIT(id);
	reg = readl(reg_addr + EARC);
	writel(reg & ~mask, reg_addr + EARC);

	return 0;
}

static int imx8mp_audiomix_reset_deassert(struct reset_controller_dev *rcdev,
					  unsigned long id)
{
	struct imx8mp_audiomix_reset_priv *priv = container_of(rcdev,
					struct imx8mp_audiomix_reset_priv, rcdev);
	void __iomem *reg_addr = priv->base;
	unsigned int mask, reg;

	if (id >= fls(EARC_RESET_MASK))
		return -EINVAL;

	mask = BIT(id);
	reg = readl(reg_addr + EARC);
	writel(reg | mask, reg_addr + EARC);

	return 0;
}

static const struct reset_control_ops imx8mp_audiomix_reset_ops = {
	.assert   = imx8mp_audiomix_reset_assert,
	.deassert = imx8mp_audiomix_reset_deassert,
};

static int imx8mp_audiomix_reset_probe(struct auxiliary_device *adev,
				       const struct auxiliary_device_id *id)
{
	struct imx8mp_audiomix_reset_priv *priv;
	struct device *dev = &adev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rcdev.owner     = THIS_MODULE;
	priv->rcdev.nr_resets = fls(EARC_RESET_MASK);
	priv->rcdev.ops       = &imx8mp_audiomix_reset_ops;
	priv->rcdev.of_node   = dev->parent->of_node;
	priv->rcdev.dev	      = dev;
	priv->rcdev.of_reset_n_cells = 1;
	priv->base            = of_iomap(dev->parent->of_node, 0);

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct auxiliary_device_id imx8mp_audiomix_reset_ids[] = {
	{
		.name = "clk_imx8mp_audiomix.reset",
	},
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, imx8mp_audiomix_reset_ids);

static struct auxiliary_driver imx8mp_audiomix_reset_driver = {
	.probe		= imx8mp_audiomix_reset_probe,
	.id_table	= imx8mp_audiomix_reset_ids,
};

module_auxiliary_driver(imx8mp_audiomix_reset_driver);

MODULE_AUTHOR("Shengjiu Wang <shengjiu.wang@nxp.com>");
MODULE_DESCRIPTION("Freescale i.MX8MP Audio Block Controller reset driver");
MODULE_LICENSE("GPL");
