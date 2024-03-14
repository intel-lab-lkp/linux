// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2018 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/reset-controller.h>

#include "meson-audio-rstc.h"

struct meson_audio_reset_data {
	struct reset_controller_dev rstc;
	struct regmap *map;
	unsigned int offset;
};

static void meson_audio_reset_reg_and_bit(struct meson_audio_reset_data *rst,
					  unsigned long id,
					  unsigned int *reg,
					  unsigned int *bit)
{
	unsigned int stride = regmap_get_reg_stride(rst->map);

	*reg = (id / (stride * BITS_PER_BYTE)) * stride;
	*reg += rst->offset;
	*bit = id % (stride * BITS_PER_BYTE);
}

static int meson_audio_reset_update(struct reset_controller_dev *rcdev,
				    unsigned long id, bool assert)
{
	struct meson_audio_reset_data *rst =
		container_of(rcdev, struct meson_audio_reset_data, rstc);
	unsigned int offset, bit;

	meson_audio_reset_reg_and_bit(rst, id, &offset, &bit);

	regmap_update_bits(rst->map, offset, BIT(bit),
			assert ? BIT(bit) : 0);

	return 0;
}

static int meson_audio_reset_status(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	struct meson_audio_reset_data *rst =
		container_of(rcdev, struct meson_audio_reset_data, rstc);
	unsigned int val, offset, bit;

	meson_audio_reset_reg_and_bit(rst, id, &offset, &bit);

	regmap_read(rst->map, offset, &val);

	return !!(val & BIT(bit));
}

static int meson_audio_reset_assert(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	return meson_audio_reset_update(rcdev, id, true);
}

static int meson_audio_reset_deassert(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	return meson_audio_reset_update(rcdev, id, false);
}

static int meson_audio_reset_toggle(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	int ret;

	ret = meson_audio_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return meson_audio_reset_deassert(rcdev, id);
}

static const struct reset_control_ops meson_audio_rstc_ops = {
	.assert = meson_audio_reset_assert,
	.deassert = meson_audio_reset_deassert,
	.reset = meson_audio_reset_toggle,
	.status = meson_audio_reset_status,
};

int meson_audio_rstc_register(struct device *dev, struct regmap *map,
			      unsigned int offset, unsigned int num)
{
	struct meson_audio_reset_data *rst;

	rst = devm_kzalloc(dev, sizeof(*rst), GFP_KERNEL);
	if (!rst)
		return -ENOMEM;

	rst->map = map;
	rst->offset = offset;
	rst->rstc.nr_resets = num;
	rst->rstc.ops = &meson_audio_rstc_ops;
	rst->rstc.of_node = dev->of_node;
	rst->rstc.owner = THIS_MODULE;

	return devm_reset_controller_register(dev, &rst->rstc);
}
EXPORT_SYMBOL_GPL(meson_audio_rstc_register);

MODULE_LICENSE("GPL v2");
