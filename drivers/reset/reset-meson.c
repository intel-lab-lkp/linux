// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Amlogic Meson Reset Controller driver
 *
 * Copyright (c) 2016 BayLibre, SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/types.h>

struct meson_reset_param {
	unsigned int reset_num;
	int reset_offset;
	int level_offset;
	bool level_low_reset;
};

struct meson_reset {
	const struct meson_reset_param *param;
	struct reset_controller_dev rcdev;
	struct regmap *map;
};

static void meson_reset_offset_and_bit(struct meson_reset *data,
				       unsigned long id,
				       unsigned int *offset,
				       unsigned int *bit)
{
	unsigned int stride = regmap_get_reg_stride(data->map);

	*offset = (id / (stride * BITS_PER_BYTE)) * stride;
	*bit = id % (stride * BITS_PER_BYTE);
}

static int meson_reset_reset(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct meson_reset *data =
		container_of(rcdev, struct meson_reset, rcdev);
	unsigned int offset, bit;

	meson_reset_offset_and_bit(data, id, &offset, &bit);
	offset += data->param->reset_offset;

	return regmap_update_bits(data->map, offset,
				  BIT(bit), BIT(bit));
}

static int meson_reset_level(struct reset_controller_dev *rcdev,
			    unsigned long id, bool assert)
{
	struct meson_reset *data =
		container_of(rcdev, struct meson_reset, rcdev);
	unsigned int offset, bit;

	meson_reset_offset_and_bit(data, id, &offset, &bit);
	offset += data->param->level_offset;
	assert ^= data->param->level_low_reset;

	return regmap_update_bits(data->map, offset,
				  BIT(bit), assert ? BIT(bit) : 0);
}

static int meson_reset_status(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	struct meson_reset *data =
		container_of(rcdev, struct meson_reset, rcdev);
	unsigned int val, offset, bit;

	meson_reset_offset_and_bit(data, id, &offset, &bit);
	offset += data->param->level_offset;

	regmap_read(data->map, offset, &val);
	val = !!(BIT(bit) & val);


	return val ^ data->param->level_low_reset;
}

static int meson_reset_assert(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	return meson_reset_level(rcdev, id, true);
}

static int meson_reset_deassert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	return meson_reset_level(rcdev, id, false);
}

static const struct reset_control_ops meson_reset_ops = {
	.reset		= meson_reset_reset,
	.assert		= meson_reset_assert,
	.deassert	= meson_reset_deassert,
	.status		= meson_reset_status,
};

static int meson_reset_probe(struct device *dev, struct regmap *map,
			     const struct meson_reset_param *param)
{
	struct meson_reset *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->param = param;
	data->map = map;
	data->rcdev.owner = dev->driver->owner;
	data->rcdev.nr_resets = param->reset_num;
	data->rcdev.ops = &meson_reset_ops;
	data->rcdev.of_node = dev->of_node;

	return devm_reset_controller_register(dev, &data->rcdev);
}

static const struct meson_reset_param meson8b_param = {
	.reset_num	= 256,
	.reset_offset	= 0x0,
	.level_offset	= 0x7c,
	.level_low_reset = true,
};

static const struct meson_reset_param meson_a1_param = {
	.reset_num	= 96,
	.reset_offset	= 0x0,
	.level_offset	= 0x40,
	.level_low_reset = true,
};

static const struct meson_reset_param meson_s4_param = {
	.reset_num	= 192,
	.reset_offset	= 0x0,
	.level_offset	= 0x40,
	.level_low_reset = true,
};

static const struct of_device_id meson_reset_dt_ids[] = {
	 { .compatible = "amlogic,meson8b-reset",    .data = &meson8b_param},
	 { .compatible = "amlogic,meson-gxbb-reset", .data = &meson8b_param},
	 { .compatible = "amlogic,meson-axg-reset",  .data = &meson8b_param},
	 { .compatible = "amlogic,meson-a1-reset",   .data = &meson_a1_param},
	 { .compatible = "amlogic,meson-s4-reset",   .data = &meson_s4_param},
	 { .compatible = "amlogic,c3-reset",   .data = &meson_s4_param},
	 { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, meson_reset_dt_ids);

static const struct regmap_config regmap_config = {
	.reg_bits   = 32,
	.val_bits   = 32,
	.reg_stride = 4,
};

static int meson_reset_pltf_probe(struct platform_device *pdev)
{

	const struct meson_reset_param *param;
	struct device *dev = &pdev->dev;
	struct regmap *map;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	param = of_device_get_match_data(dev);
	if (!param)
		return -ENODEV;

	map = devm_regmap_init_mmio(dev, base, &regmap_config);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map),
				     "can't init regmap mmio region\n");

	return meson_reset_probe(dev, map, param);
}

static struct platform_driver meson_reset_pltf_driver = {
	.probe	= meson_reset_pltf_probe,
	.driver = {
		.name		= "meson_reset",
		.of_match_table	= meson_reset_dt_ids,
	},
};
module_platform_driver(meson_reset_pltf_driver);

MODULE_DESCRIPTION("Amlogic Meson Reset Controller driver");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_LICENSE("Dual BSD/GPL");
