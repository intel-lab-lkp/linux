// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * CIX System Reset Controller (SRC) driver
 *
 * Author: Jerry Zhu <jerry.zhu@cixtech.com>
 */

#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/reset/sky1.h>
#include <linux/regmap.h>

#define SKY1_RESET_SLEEP_MIN_US		50
#define SKY1_RESET_SLEEP_MAX_US		100

struct sky1_src {
	struct reset_controller_dev rcdev;
	const struct sky1_src_signal *signals;
	struct regmap *regmap;
};

static struct sky1_src *to_sky1_src(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct sky1_src, rcdev);
}

static int sky1_reset_set(struct reset_controller_dev *rcdev,
			  unsigned long id, bool assert)
{
	struct sky1_src *sky1src = to_sky1_src(rcdev);
	const struct sky1_src_signal *signal = &sky1src->signals[id];
	unsigned int value = assert ? 0 : sky1src->signals[id].bit;

	return regmap_update_bits(sky1src->regmap,
				  signal->offset, signal->bit, value);
}

static int sky1_reset_assert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	sky1_reset_set(rcdev, id, true);
	usleep_range(SKY1_RESET_SLEEP_MIN_US,
		     SKY1_RESET_SLEEP_MAX_US);
	return 0;
}

static int sky1_reset_deassert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	sky1_reset_set(rcdev, id, false);
	usleep_range(SKY1_RESET_SLEEP_MIN_US,
		     SKY1_RESET_SLEEP_MAX_US);
	return 0;
}

static int sky1_reset(struct reset_controller_dev *rcdev,
		      unsigned long id)
{
	sky1_reset_assert(rcdev, id);
	sky1_reset_deassert(rcdev, id);
	return 0;
}

static int sky1_reset_status(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	unsigned int value = 0;
	struct sky1_src *sky1src = to_sky1_src(rcdev);
	const struct sky1_src_signal *signal = &sky1src->signals[id];

	regmap_read(sky1src->regmap, signal->offset, &value);
	return !(value & signal->bit);
}

static const struct reset_control_ops sky1_src_ops = {
	.reset    = sky1_reset,
	.assert   = sky1_reset_assert,
	.deassert = sky1_reset_deassert,
	.status   = sky1_reset_status
};

int sky1_reset_common_probe(struct platform_device *pdev,
			const struct sky1_src_variant *variant)
{
	struct sky1_src *sky1src;
	struct device *dev = &pdev->dev;

	sky1src = devm_kzalloc(dev, sizeof(*sky1src), GFP_KERNEL);
	if (!sky1src)
		return -ENOMEM;

	sky1src->regmap = device_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(sky1src->regmap)) {
		dev_err(dev, "Unable to get sky1-src regmap");
		return PTR_ERR(sky1src->regmap);
	}

	sky1src->signals = variant->signals;
	sky1src->rcdev.owner     = THIS_MODULE;
	sky1src->rcdev.nr_resets = variant->signals_num;
	sky1src->rcdev.ops       = &sky1_src_ops;
	sky1src->rcdev.of_node   = dev->parent->of_node;
	sky1src->rcdev.dev       = dev;

	return devm_reset_controller_register(dev, &sky1src->rcdev);
}
EXPORT_SYMBOL_GPL(sky1_reset_common_probe);

MODULE_AUTHOR("Jerry Zhu <jerry.zhu@cixtech.com>");
MODULE_DESCRIPTION("Cix Sky1 reset driver");
MODULE_LICENSE("GPL");
