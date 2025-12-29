// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 */

#include <linux/of.h>
#include <linux/device.h>
#include "reset.h"

#define RTK_RESET_BANK_SHIFT         8
#define RTK_RESET_ID_MASK            0xff
#define to_rtk_reset_controller(r) container_of(r, struct rtk_reset_data, rcdev)

static inline struct rtk_reset_bank *
rtk_reset_get_bank(struct rtk_reset_data *data, unsigned long idx)
{
	int bank_id = idx >> RTK_RESET_BANK_SHIFT;

	return &data->banks[bank_id];
}

static inline int rtk_reset_get_id(struct rtk_reset_data *data,
				   unsigned long idx)
{
	return idx & RTK_RESET_ID_MASK;
}

static int rtk_reset_update_bits(struct rtk_reset_data *data, u32 offset,
				 u32 mask, u32 val)
{
	int ret;

	ret = regmap_update_bits(data->regmap, offset, mask, val);
	return ret;
}

static int rtk_reset_read(struct rtk_reset_data *data, u32 offset, u32 *val)
{
	int ret;

	ret = regmap_read(data->regmap, offset, val);
	return ret;
}

static int rtk_reset_assert(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = rtk_reset_get_bank(data, idx);
	u32 id = rtk_reset_get_id(data, idx);
	u32 mask = bank->write_en ? (0x3 << id) : BIT(id);
	u32 val = bank->write_en ? (0x2 << id) : 0;

	return rtk_reset_update_bits(data, bank->ofs, mask, val);
}

static int rtk_reset_deassert(struct reset_controller_dev *rcdev,
			      unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = rtk_reset_get_bank(data, idx);
	u32 id = rtk_reset_get_id(data, idx);
	u32 mask = bank->write_en ? (0x3 << id) : BIT(id);
	u32 val = mask;

	return rtk_reset_update_bits(data, bank->ofs, mask, val);
}

static int rtk_reset_reset(struct reset_controller_dev *rcdev,
			   unsigned long idx)
{
	int ret;

	ret = rtk_reset_assert(rcdev, idx);
	if (ret)
		return ret;
	return rtk_reset_deassert(rcdev, idx);
}

static int rtk_reset_status(struct reset_controller_dev *rcdev,
			    unsigned long idx)
{
	struct rtk_reset_data *data = to_rtk_reset_controller(rcdev);
	struct rtk_reset_bank *bank = &data->banks[idx >> 8];
	u32 id = idx & 0xff;
	u32 val;

	rtk_reset_read(data, bank->ofs, &val);

	return !((val >> id) & 1);
}

static const struct reset_control_ops rtk_reset_ops = {
	.reset    = rtk_reset_reset,
	.assert   = rtk_reset_assert,
	.deassert = rtk_reset_deassert,
	.status   = rtk_reset_status,
};

static int rtk_of_reset_xlate(struct reset_controller_dev *rcdev,
			      const struct of_phandle_args *reset_spec)
{
	int val;

	val = reset_spec->args[0];

	if (val >= rcdev->nr_resets)
		return -EINVAL;

	return val;
}

int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_initdata *initdata)
{
	struct rtk_reset_data *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->num_banks = initdata->num_banks;
	data->banks = initdata->banks;
	data->regmap = initdata->regmap;
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.ops = &rtk_reset_ops;
	data->rcdev.dev = dev;
	data->rcdev.of_node = dev->of_node;
	data->rcdev.nr_resets = initdata->num_banks * 0x100;
	data->rcdev.of_xlate = rtk_of_reset_xlate;
	data->rcdev.of_reset_n_cells = 1;

	return devm_reset_controller_register(dev, &data->rcdev);
}
EXPORT_SYMBOL_GPL(rtk_reset_controller_add);

