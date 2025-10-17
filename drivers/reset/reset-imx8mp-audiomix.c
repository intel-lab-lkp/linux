// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <dt-bindings/reset/imx8mp-reset-audiomix.h>

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define IMX8MP_AUDIOMIX_EARC_RESET_OFFSET	0x200
#define IMX8MP_AUDIOMIX_DSP_RUNSTALL_OFFSET	0x108

struct imx8mp_reset_map {
	unsigned int offset;
	unsigned int mask;
	unsigned int shift;
	bool active_low;
};

struct imx8mp_reset_info {
	const struct imx8mp_reset_map *map;
	int num_lines;
};

static const struct imx8mp_reset_map imx8mp_reset_map[] = {
	[IMX8MP_AUDIOMIX_EARC_RESET] = {
		.offset	= IMX8MP_AUDIOMIX_EARC_RESET_OFFSET,
		.mask = BIT(0),
		.shift = 0,
		.active_low = true,
	},
	[IMX8MP_AUDIOMIX_EARC_PHY_RESET] = {
		.offset	= IMX8MP_AUDIOMIX_EARC_RESET_OFFSET,
		.mask = BIT(1),
		.shift = 1,
		.active_low = true,
	},
	[IMX8MP_AUDIOMIX_DSP_RUNSTALL] = {
		.offset	= IMX8MP_AUDIOMIX_DSP_RUNSTALL_OFFSET,
		.mask = BIT(5),
		.shift = 5,
		.active_low = false,
	},
};

static const struct imx8mp_reset_info imx8mp_reset_info = {
	.map = imx8mp_reset_map,
	.num_lines = ARRAY_SIZE(imx8mp_reset_map),
};

struct imx8mp_audiomix_reset {
	struct reset_controller_dev rcdev;
	void __iomem *base;
	struct regmap *regmap;
	const struct imx8mp_reset_info *rinfo;
};

static struct imx8mp_audiomix_reset *to_imx8mp_audiomix_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct imx8mp_audiomix_reset, rcdev);
}

static int imx8mp_audiomix_update(struct reset_controller_dev *rcdev,
				  unsigned long id, bool assert)
{
	struct imx8mp_audiomix_reset *priv = to_imx8mp_audiomix_reset(rcdev);
	const struct imx8mp_reset_map *reset_map = priv->rinfo->map;
	unsigned int mask, offset, active_low, shift, val;

	mask = reset_map[id].mask;
	offset = reset_map[id].offset;
	active_low = reset_map[id].active_low;
	shift = reset_map[id].shift;
	val = (active_low ^ assert) << shift;

	return regmap_update_bits(priv->regmap, offset, mask, val);
}

static int imx8mp_audiomix_reset_assert(struct reset_controller_dev *rcdev,
					unsigned long id)
{
	return imx8mp_audiomix_update(rcdev, id, true);
}

static int imx8mp_audiomix_reset_deassert(struct reset_controller_dev *rcdev,
					  unsigned long id)
{
	return imx8mp_audiomix_update(rcdev, id, false);
}

static const struct reset_control_ops imx8mp_audiomix_reset_ops = {
	.assert   = imx8mp_audiomix_reset_assert,
	.deassert = imx8mp_audiomix_reset_deassert,
};

static const struct regmap_config regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

/* assumption: registered only if not using parent regmap */
static void imx8mp_audiomix_reset_iounmap(void *data)
{
	struct imx8mp_audiomix_reset *priv = dev_get_drvdata(data);

	iounmap(priv->base);
}

/* assumption: dev_set_drvdata() is called before this */
static int imx8mp_audiomix_reset_get_regmap(struct device *dev)
{
	struct imx8mp_audiomix_reset *priv;
	int ret;

	priv = dev_get_drvdata(dev);

	/* try to use the parent's regmap */
	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (priv->regmap)
		return 0;

	/* ... if that's not possible then initialize the regmap right now */
	priv->base = of_iomap(dev->parent->of_node, 0);
	if (!priv->base)
		return dev_err_probe(dev, -ENOMEM, "failed to iomap address space\n");

	ret = devm_add_action_or_reset(dev, imx8mp_audiomix_reset_iounmap, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register action\n");

	priv->regmap = devm_regmap_init_mmio(dev, priv->base, &regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to initialize regmap\n");

	return 0;
}

static int imx8mp_audiomix_reset_probe(struct auxiliary_device *adev,
				       const struct auxiliary_device_id *id)
{
	struct imx8mp_audiomix_reset *priv;
	struct device *dev = &adev->dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rcdev.owner     = THIS_MODULE;
	priv->rinfo           = (const struct imx8mp_reset_info *)id->driver_data;
	priv->rcdev.nr_resets = priv->rinfo->num_lines;
	priv->rcdev.ops       = &imx8mp_audiomix_reset_ops;
	priv->rcdev.of_node   = dev->parent->of_node;
	priv->rcdev.dev	      = dev;
	priv->rcdev.of_reset_n_cells = 1;

	/* keep before call to imx8mp_audiomix_reset_init_regmap() */
	dev_set_drvdata(dev, priv);

	ret = imx8mp_audiomix_reset_get_regmap(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regmap\n");

	ret = devm_reset_controller_register(dev, &priv->rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register reset controller\n");

	return 0;
}

static const struct auxiliary_device_id imx8mp_audiomix_reset_ids[] = {
	{
		.name = "clk_imx8mp_audiomix.reset",
		.driver_data = (kernel_ulong_t)&imx8mp_reset_info,
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
