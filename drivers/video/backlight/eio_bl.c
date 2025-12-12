// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for Advantech EIO Embedded controller.
 *
 * Copyright (C) 2025 Advantech Corporation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/errno.h>
#include <linux/mfd/core.h>
#include <linux/mfd/eio.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define PMC_BL_WRITE		0x20
#define PMC_BL_READ		0x21

#define BL_CTRL_STATUS		0x00
#define BL_CTRL_ENABLE		0x12
#define BL_CTRL_ENABLE_INVERT	0x13
#define BL_CTRL_DUTY		0x14
#define BL_CTRL_INVERT		0x15
#define BL_CTRL_FREQ		0x16

#define BL_MAX			2

#define BL_STATUS_AVAIL		0x01
#define BL_ENABLE_OFF		0x00
#define BL_ENABLE_ON		0x01
#define BL_ENABLE_AUTO		BIT(1)

#define USE_DEFAULT		-1
#define THERMAL_MAX		100

#define BL_AVAIL		BIT(0)
#define BL_PWM_DC		BIT(1)
#define BL_PWM_SRC		BIT(2)
#define BL_BRI_INVERT		BIT(3)
#define BL_ENABLE_PIN_SUPP	BIT(4)
#define BL_POWER_INVERT		BIT(5)
#define BL_ENABLE_PIN_EN	BIT(6)
#define BL_FIRMWARE_ERROR	BIT(7)

static uint bri_freq = USE_DEFAULT;
module_param(bri_freq, uint, 0444);
MODULE_PARM_DESC(bri_freq, "Setup backlight PWM frequency.\n");

static int bri_invert = USE_DEFAULT;
module_param(bri_invert, int, 0444);
MODULE_PARM_DESC(bri_invert, "Setup backlight PWM polarity.\n");

static int bl_power_invert = USE_DEFAULT;
module_param(bl_power_invert, int, 0444);
MODULE_PARM_DESC(bl_power_invert, "Setup backlight enable pin polarity.\n");

static int timeout;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Set PMC command timeout value.\n");

struct eio_bl_dev {
	struct device *mfd;
	u8 id;
	u8 max;
};

static int pmc_write(struct device *mfd, u8 ctrl, u8 dev_id, void *data)
{
	struct pmc_op op = {
		.cmd       = PMC_BL_WRITE,
		.control   = ctrl,
		.device_id = dev_id,
		.payload   = (u8 *)data,
		.size      = (ctrl == BL_CTRL_FREQ) ? 4 : 1,
		.timeout   = timeout,
	};

	return eio_core_pmc_operation(mfd, &op);
}

static int pmc_read(struct device *mfd, u8 ctrl, u8 dev_id, void *data)
{
	struct pmc_op op = {
		.cmd       = PMC_BL_READ,
		.control   = ctrl,
		.device_id = dev_id,
		.payload   = (u8 *)data,
		.size      = (ctrl == BL_CTRL_FREQ) ? 4 : 1,
		.timeout   = timeout,
	};

	return eio_core_pmc_operation(mfd, &op);
}

static int bl_update_status(struct backlight_device *bl)
{
	struct eio_bl_dev *eio_bl = bl_get_data(bl);
	u32 max  = bl->props.max_brightness;
	u8 duty = clamp_val(bl->props.brightness, 0, max);
	u8 sw = bl->props.power == BACKLIGHT_POWER_OFF;
	int ret;

	/* Setup PWM duty */
	ret = pmc_write(eio_bl->mfd, BL_CTRL_DUTY, eio_bl->id, &duty);
	if (ret)
		return ret;

	/* Setup backlight enable pin */
	return pmc_write(eio_bl->mfd, BL_CTRL_ENABLE, eio_bl->id, &sw);
}

static int bl_get_brightness(struct backlight_device *bl)
{
	struct eio_bl_dev *eio_bl = bl_get_data(bl);
	u8 duty = 0;
	int ret;

	ret = pmc_read(eio_bl->mfd, BL_CTRL_DUTY, eio_bl->id, &duty);

	if (ret)
		return ret;

	return duty;
}

static const struct backlight_ops bl_ops = {
	.get_brightness = bl_get_brightness,
	.update_status	= bl_update_status,
	.options	= BL_CORE_SUSPENDRESUME,
};

static int bl_init(struct device *dev, int id,
		   struct backlight_properties *props)
{
	int ret;
	u8 enabled = 0;
	u8 status = 0;

	/* Check EC-supported backlight */
	ret = pmc_read(dev, BL_CTRL_STATUS, id, &status);
	if (ret)
		return ret;

	if (!(status & BL_STATUS_AVAIL)) {
		dev_dbg(dev, "eio_bl%d hardware report disabled.\n", id);
		return -ENXIO;
	}

	ret = pmc_read(dev, BL_CTRL_DUTY, id, &props->brightness);
	if (ret)
		return ret;

	/* Invert PWM */
	dev_dbg(dev, "bri_invert=%d\n", bri_invert);
	if (bri_invert > USE_DEFAULT) {
		ret = pmc_write(dev, BL_CTRL_INVERT, id, &bri_invert);
		if (ret)
			return ret;
	}

	bri_invert = 0;
	ret = pmc_read(dev, BL_CTRL_INVERT, id, &bri_invert);
	if (ret)
		return ret;

	dev_dbg(dev, "bri_freq=%u\n", bri_freq);
	if (bri_freq != USE_DEFAULT) {
		ret = pmc_write(dev, BL_CTRL_FREQ, id, &bri_freq);
		if (ret)
			return ret;
	}

	ret = pmc_read(dev, BL_CTRL_FREQ, id, &bri_freq);
	if (ret)
		return ret;

	dev_dbg(dev, "bl_power_invert=%d\n", bl_power_invert);
	if (bl_power_invert >= USE_DEFAULT) {
		ret = pmc_write(dev, BL_CTRL_ENABLE_INVERT, id, &bl_power_invert);
		if (ret)
			return ret;
	}

	bl_power_invert = 0;
	ret = pmc_read(dev, BL_CTRL_ENABLE_INVERT, id, &bl_power_invert);
	if (ret)
		return ret;

	/* Read power state */
	ret = pmc_read(dev, BL_CTRL_ENABLE, id, &enabled);
	if (ret)
		return ret;

	props->power = enabled ? BACKLIGHT_POWER_OFF : BACKLIGHT_POWER_ON;

	return 0;
}

static int bl_probe(struct platform_device *pdev)
{
	u8 id;
	struct device *dev = &pdev->dev;
	struct eio_dev *eio_dev = dev_get_drvdata(dev->parent);

	if (!eio_dev) {
		dev_err(dev, "eio_core not present\n");
		return -ENODEV;
	}

	for (id = 0; id < BL_MAX; id++) {
		char name[32];
		struct backlight_properties props;
		struct eio_bl_dev *eio_bl;
		struct backlight_device *bl;
		int ret;

		memset(&props, 0, sizeof(props));
		props.type           = BACKLIGHT_RAW;
		props.max_brightness = THERMAL_MAX;
		props.power          = BACKLIGHT_POWER_OFF;
		props.brightness     = props.max_brightness;

		eio_bl = devm_kzalloc(dev, sizeof(*eio_bl), GFP_KERNEL);
		if (!eio_bl)
			return -ENOMEM;

		eio_bl->mfd = dev->parent;
		eio_bl->id  = id;
		eio_bl->max = props.max_brightness;

		ret = bl_init(eio_bl->mfd, id, &props);
		if (ret) {
			dev_info(dev, "%d No Backlight %u enabled!\n", ret, id);
			continue;
		}

		snprintf(name, sizeof(name), "%s%u", pdev->name, id);

		bl = devm_backlight_device_register(dev, name, dev, eio_bl,
						    &bl_ops, &props);

		if (IS_ERR(bl)) {
			ret = PTR_ERR(bl);
			if (ret == -EPROBE_DEFER)
				return ret;

			dev_err(dev, "register %s failed: %d\n", name, ret);
			continue;
		}

		dev_info(dev, "%s registered (max=%u)\n", name, props.max_brightness);
	}

	return 0;
}

static struct platform_driver bl_driver = {
	.probe  = bl_probe,
	.driver = {
		.name = "eio_bl",
	},
};

module_platform_driver(bl_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Backlight driver for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");
