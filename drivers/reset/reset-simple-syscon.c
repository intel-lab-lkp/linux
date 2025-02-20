// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Simple SYSCON Reset Controller Driver
 *
 * Copyright (C) 2025 Marvell, Wilson Ding <dingwei@marvell.com>
 *
 * Based on Simple Reset Controller Driver
 *
 * Copyright (C) 2017 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 * Copyright 2013 Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/reset/reset-simple.h>
#include <linux/spinlock.h>

static inline struct reset_simple_data *
to_reset_simple_data(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct reset_simple_data, rcdev);
}

static int reset_simple_syscon_update(struct reset_controller_dev *rcdev,
				      unsigned long id, bool assert)
{
	struct reset_simple_data *data = to_reset_simple_data(rcdev);
	int reg_width = sizeof(u32);
	int bank = id / (reg_width * BITS_PER_BYTE);
	int offset = id % (reg_width * BITS_PER_BYTE);
	u32 mask, val;

	mask = BIT(offset);

	if (assert ^ data->active_low)
		val = mask;
	else
		val = 0;

	return regmap_write_bits(data->regmap,
				 data->reg_offset + (bank * reg_width),
				 mask, val);
}

static int reset_simple_syscon_assert(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	return reset_simple_syscon_update(rcdev, id, true);
}

static int reset_simple_syscon_deassert(struct reset_controller_dev *rcdev,
					unsigned long id)
{
	return reset_simple_syscon_update(rcdev, id, false);
}

static int reset_simple_syscon_reset(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct reset_simple_data *data = to_reset_simple_data(rcdev);
	int ret;

	if (!data->reset_us)
		return -EINVAL;

	ret = reset_simple_syscon_assert(rcdev, id);
	if (ret)
		return ret;

	usleep_range(data->reset_us, data->reset_us * 2);

	return reset_simple_syscon_deassert(rcdev, id);
}

static int reset_simple_syscon_status(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	struct reset_simple_data *data = to_reset_simple_data(rcdev);
	int reg_width = sizeof(u32);
	int bank = id / (reg_width * BITS_PER_BYTE);
	int offset = id % (reg_width * BITS_PER_BYTE);
	u32 reg;
	int ret;

	ret = regmap_read(data->regmap, data->reg_offset + (bank * reg_width),
			  &reg);
	if (ret)
		return ret;

	return !(reg & BIT(offset)) ^ !data->status_active_low;
}

const struct reset_control_ops reset_simple_syscon_ops = {
	.assert		= reset_simple_syscon_assert,
	.deassert	= reset_simple_syscon_deassert,
	.reset		= reset_simple_syscon_reset,
	.status		= reset_simple_syscon_status,
};
EXPORT_SYMBOL_GPL(reset_simple_syscon_ops);

/**
 * struct reset_simple_syscon_devdata - simple reset controller properties
 * @reg_offset: offset between base address and first reset register.
 * @nr_resets: number of resets. If not set, default to resource size in bits.
 * @active_low: if true, bits are cleared to assert the reset. Otherwise, bits
 *              are set to assert the reset.
 * @status_active_low: if true, bits read back as cleared while the reset is
 *                     asserted. Otherwise, bits read back as set while the
 *                     reset is asserted.
 */
struct reset_simple_syscon_devdata {
	u32 reg_offset;
	u32 nr_resets;
	bool active_low;
	bool status_active_low;
};

static const struct reset_simple_syscon_devdata reset_simple_syscon_armada8k = {
	.nr_resets = 32,
	.active_low = true,
	.status_active_low = true,
};

static const struct of_device_id reset_simple_syscon_dt_ids[] = {
	{ .compatible = "marvell,armada8k-reset",
		.data = &reset_simple_syscon_armada8k },
	{ /* sentinel */ },
};

static int reset_simple_syscon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct reset_simple_syscon_devdata *devdata;
	struct reset_simple_data *data;
	u32 reg[2];

	devdata = of_device_get_match_data(dev);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	/*
	 * If the "reg" property is available, retrieve the reg_offset and
	 * calculate the nr_resets based on the register size. If "reg" is not
	 * available, attempt to read the "offset" property for reg_offset and
	 * use nr_resets from devdata. If "offset" is also not available,
	 * default to using reg_offset from devdata.
	 */
	if (device_property_read_u32_array(&pdev->dev, "reg", reg, 2)) {
		if (device_property_read_u32(&pdev->dev, "offset",
					     &data->reg_offset))
			data->reg_offset = devdata->reg_offset;
		data->rcdev.nr_resets = devdata->nr_resets;
	} else {
		data->reg_offset = reg[0];
		data->rcdev.nr_resets = reg[1] * BITS_PER_BYTE;
	}

	data->active_low = devdata->active_low;
	data->status_active_low = devdata->status_active_low;

	if (data->rcdev.nr_resets == 0) {
		dev_err(dev, "no reset lines\n");
		return -EINVAL;
	}

	spin_lock_init(&data->lock);
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.ops = &reset_simple_syscon_ops;
	data->rcdev.of_node = dev->of_node;

	return devm_reset_controller_register(dev, &data->rcdev);
}

static struct platform_driver reset_simple_syscon_driver = {
	.probe	= reset_simple_syscon_probe,
	.driver = {
		.name		= "simple-reset-syscon",
		.of_match_table	= reset_simple_syscon_dt_ids,
	},
};
builtin_platform_driver(reset_simple_syscon_driver);
