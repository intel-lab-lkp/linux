// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek DHC I/O Level Detect driver
 *
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

struct rtd_io_detect_desc_info {
	const char *name;
	const char *state_1v8;
	const char *state_3v3;
	unsigned int reg_offset;
	unsigned int en_offset;
	unsigned int status_offset;
};

struct rtd_io_detect_descs {
	const struct rtd_io_detect_desc_info *info;
	int num_descs;
};

struct rtd_io_detect_data {
	const struct rtd_io_detect_descs *descs;
	struct regmap *base;
	struct device *dev;
};

#define RTD_IO_DETECT_DESC(_name, _reg_off, _en_off, _st_off) \
	{ \
		.name = #_name, \
		.state_1v8 = #_name "_1v8", \
		.state_3v3 = #_name "_3v3", \
		.reg_offset = _reg_off, \
		.en_offset = _en_off, \
		.status_offset = _st_off, \
	}

static const struct rtd_io_detect_desc_info rtd1625_io_detect_desc[] = {
	RTD_IO_DETECT_DESC(rgmii, 0x1a0, 8, 1),
	RTD_IO_DETECT_DESC(sd, 0x1a0, 9, 2),
	RTD_IO_DETECT_DESC(csi, 0x1a0, 10, 3),
	RTD_IO_DETECT_DESC(sdio, 0x1a0, 11, 4),
	RTD_IO_DETECT_DESC(uart1, 0x1a0, 12, 5),
	RTD_IO_DETECT_DESC(aio, 0x1a0, 13, 6),
	RTD_IO_DETECT_DESC(emmc, 0x1a0, 14, 7),
};

static const struct rtd_io_detect_descs rtd1625_io_detect_descs = {
	.info = rtd1625_io_detect_desc,
	.num_descs = ARRAY_SIZE(rtd1625_io_detect_desc),
};

static void detect_io_set(struct pinctrl *pinctrl,
			  const struct rtd_io_detect_desc_info *desc,
			  struct rtd_io_detect_data *data)
{
	struct pinctrl_state *state_1v8;
	struct pinctrl_state *state_3v3;
	unsigned int val;
	int ret;

	state_1v8 = pinctrl_lookup_state(pinctrl, desc->state_1v8);
	if (IS_ERR(state_1v8)) {
		dev_err(data->dev, "Failed to lookup %s state: %ld\n",
			desc->state_1v8, PTR_ERR(state_1v8));
		return;
	}

	state_3v3 = pinctrl_lookup_state(pinctrl, desc->state_3v3);
	if (IS_ERR(state_3v3)) {
		dev_err(data->dev, "Failed to lookup %s state: %ld\n",
			desc->state_3v3, PTR_ERR(state_3v3));
		return;
	}

	regmap_update_bits(data->base, desc->reg_offset,
			   BIT(desc->en_offset), BIT(desc->en_offset));

	regmap_read(data->base, desc->reg_offset, &val);

	ret = pinctrl_select_state(pinctrl,
				   (val & BIT(desc->status_offset)) ? state_3v3 : state_1v8);
	if (ret)
		dev_err(data->dev, "Failed to select pinctrl state\n");
}

static int rtd_io_detect_probe(struct platform_device *pdev)
{
	struct rtd_io_detect_data *data;
	struct device *dev = &pdev->dev;
	struct device_node *pinctrl_np;
	struct pinctrl *pinctrl;
	int i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	pinctrl_np = of_parse_phandle(dev->of_node, "realtek,iso-pinctrl", 0);
	if (!pinctrl_np) {
		dev_err(dev, "Failed to find ISO pinctrl node\n");
		return -ENODEV;
	}

	data->base = device_node_to_regmap(pinctrl_np);
	of_node_put(pinctrl_np);

	if (IS_ERR(data->base))
		return dev_err_probe(dev, PTR_ERR(data->base), "Failed to get regmap\n");

	data->descs = device_get_match_data(dev);
	if (!data->descs)
		return -EINVAL;

	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(dev, PTR_ERR(pinctrl), "Failed to get pinctrl\n");

	data->dev = dev;

	for (i = 0; i < data->descs->num_descs; i++)
		detect_io_set(pinctrl, &data->descs->info[i], data);

	return 0;
}

static const struct of_device_id rtd_io_detect_of_matches[] = {
	{ .compatible = "realtek,rtd1625-io-detect", .data = &rtd1625_io_detect_descs },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtd_io_detect_of_matches);

static struct platform_driver rtd_io_detect_driver = {
	.driver = {
		.name = "rtd_io_level_detect",
		.of_match_table = rtd_io_detect_of_matches,
	},
	.probe = rtd_io_detect_probe,
};
module_platform_driver(rtd_io_detect_driver);

MODULE_DESCRIPTION("Realtek DHC SoC I/O Level Detect driver");
MODULE_LICENSE("GPL");

