// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include "common.h"

static inline struct rtk_reset_data *to_rtk_reset_controller(struct reset_controller_dev *r)
{
	return container_of(r, struct rtk_reset_data, rcdev);
}

static inline struct rtk_reset_desc *rtk_reset_get_desc(struct rtk_reset_data *data,
							unsigned long idx)
{
	return &data->descs[idx];
}

static int rtk_reset_assert(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_desc *desc = rtk_reset_get_desc(data, idx);
	u32 mask = desc->write_en ? (0x3 << desc->bit) : BIT(desc->bit);
	u32 val  = desc->write_en ? (0x2 << desc->bit) : 0;

	return regmap_update_bits(data->regmap, desc->ofs, mask, val);
}

static int rtk_reset_deassert(struct reset_controller_dev *rcdev,
			      unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_desc *desc = rtk_reset_get_desc(data, idx);
	u32 mask = desc->write_en ? (0x3 << desc->bit) : BIT(desc->bit);
	u32 val  = mask;

	return regmap_update_bits(data->regmap, desc->ofs, mask, val);
}

static int rtk_reset_status(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_desc *desc = rtk_reset_get_desc(data, idx);
	u32 val;
	int ret;

	ret = regmap_read(data->regmap, desc->ofs, &val);
	if (ret)
		return ret;

	return !((val >> desc->bit) & 1);
}

static const struct reset_control_ops rtk_reset_ops = {
	.assert   = rtk_reset_assert,
	.deassert = rtk_reset_deassert,
	.status   = rtk_reset_status,
};

/* The caller must initialize data->rcdev.nr_resets and data->descs before
 * calling rtk_reset_controller_add().
 */
int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_data *data)
{
	struct device *parent = dev->parent;

	data->regmap = dev_get_regmap(parent, NULL);
	if (!data->regmap)
		return -ENODEV;

	data->rcdev.owner     = THIS_MODULE;
	data->rcdev.ops       = &rtk_reset_ops;
	data->rcdev.dev       = dev;
	data->rcdev.of_node   = parent->of_node;

	return devm_reset_controller_register(dev, &data->rcdev);
}
EXPORT_SYMBOL_NS_GPL(rtk_reset_controller_add, "REALTEK_RESET");
