// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 */

#include <linux/device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include "reset.h"

#define RTK_RESET_BANK_SHIFT 5
#define RTK_RESET_ID_MASK    0x1f

struct rtk_reset_data {
	struct device *dev;
	struct reset_controller_dev rcdev;
	struct rtk_reset_bank *banks;
	struct regmap *regmap;
};

static inline struct rtk_reset_data *to_rtk_reset_controller(struct reset_controller_dev *r)
{
	return container_of(r, struct rtk_reset_data, rcdev);
}

static inline struct rtk_reset_bank *
rtk_reset_get_bank(struct rtk_reset_data *data, unsigned long idx)
{
	int bank_id = idx >> RTK_RESET_BANK_SHIFT;

	return &data->banks[bank_id];
}

static inline int rtk_reset_get_id(unsigned long idx)
{
	return idx & RTK_RESET_ID_MASK;
}

static int rtk_reset_assert(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = rtk_reset_get_bank(data, idx);
	u32 id = rtk_reset_get_id(idx);
	u32 mask = bank->write_en ? (UL(0x3) << id) : BIT(id);
	u32 val = bank->write_en ? (UL(0x2) << id) : 0;

	return regmap_update_bits(data->regmap, bank->ofs, mask, val);
}

static int rtk_reset_deassert(struct reset_controller_dev *rcdev,
			      unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = rtk_reset_get_bank(data, idx);
	u32 id = rtk_reset_get_id(idx);
	u32 mask = bank->write_en ? (0x3 << id) : BIT(id);
	u32 val = mask;

	return regmap_update_bits(data->regmap, bank->ofs, mask, val);
}

static int rtk_reset_status(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = &data->banks[idx >> RTK_RESET_BANK_SHIFT];
	u32 id = rtk_reset_get_id(idx);
	u32 val;
	int ret;

	ret = regmap_read(data->regmap, bank->ofs, &val);
	if (ret)
		return ret;

	return !((val >> id) & 1);
}

static const struct reset_control_ops rtk_reset_ops = {
	.assert   = rtk_reset_assert,
	.deassert = rtk_reset_deassert,
	.status   = rtk_reset_status,
};

int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_initdata *initdata)
{
	struct rtk_reset_data *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->banks = initdata->banks;
	data->regmap = initdata->regmap;
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.ops = &rtk_reset_ops;
	data->rcdev.dev = dev;
	data->rcdev.of_node = dev->of_node;
	data->rcdev.nr_resets = initdata->num_banks * 32;

	return devm_reset_controller_register(dev, &data->rcdev);
}
EXPORT_SYMBOL_GPL(rtk_reset_controller_add);
