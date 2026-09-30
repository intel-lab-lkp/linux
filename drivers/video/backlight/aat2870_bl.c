// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/drivers/video/backlight/aat2870_bl.c
 *
 * Copyright (c) 2011, NVIDIA Corporation.
 * Author: Jin Park <jinyoungp@nvidia.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/backlight.h>
#include <linux/mfd/aat2870.h>

/* Backlight has 8 channels, each bit represents one channel */
#define AAT2870_BL_CH_ALL	0xFF

/* Backlight current magnitude (mA) */
#define AAT2870_CURRENT_MIN	450000
#define AAT2870_CURRENT_MAX	27900000
#define AAT2870_CURRENT_STEP	900000

struct aat2870_bl_driver_data {
	struct platform_device *pdev;
	struct backlight_device *bd;

	u8 channels;
	int max_current;
	int brightness; /* current brightness */
};

static inline int aat2870_brightness(struct aat2870_bl_driver_data *aat2870_bl,
				     int brightness)
{
	struct backlight_device *bd = aat2870_bl->bd;
	int val;

	val = brightness * aat2870_bl->max_current;
	val /= bd->props.max_brightness;

	return val;
}

static inline int aat2870_bl_enable(struct aat2870_bl_driver_data *aat2870_bl)
{
	struct aat2870_data *aat2870
			= dev_get_drvdata(aat2870_bl->pdev->dev.parent);

	return aat2870->write(aat2870, AAT2870_BL_CH_EN,
			      aat2870_bl->channels);
}

static inline int aat2870_bl_disable(struct aat2870_bl_driver_data *aat2870_bl)
{
	struct aat2870_data *aat2870
			= dev_get_drvdata(aat2870_bl->pdev->dev.parent);

	return aat2870->write(aat2870, AAT2870_BL_CH_EN, 0x0);
}

static int aat2870_bl_update_status(struct backlight_device *bd)
{
	struct aat2870_bl_driver_data *aat2870_bl = bl_get_data(bd);
	struct aat2870_data *aat2870 =
			dev_get_drvdata(aat2870_bl->pdev->dev.parent);
	int brightness = backlight_get_brightness(bd);
	int ret;

	if ((brightness < 0) || (bd->props.max_brightness < brightness)) {
		dev_err(&bd->dev, "invalid brightness, %d\n", brightness);
		return -EINVAL;
	}

	dev_dbg(&bd->dev, "brightness=%d, power=%d, state=%d\n",
		 bd->props.brightness, bd->props.power, bd->props.state);

	ret = aat2870->write(aat2870, AAT2870_BLM,
			     (u8)aat2870_brightness(aat2870_bl, brightness));
	if (ret < 0)
		return ret;

	if (brightness == 0) {
		ret = aat2870_bl_disable(aat2870_bl);
		if (ret < 0)
			return ret;
	} else if (aat2870_bl->brightness == 0) {
		ret = aat2870_bl_enable(aat2870_bl);
		if (ret < 0)
			return ret;
	}

	aat2870_bl->brightness = brightness;

	return 0;
}

static const struct backlight_ops aat2870_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = aat2870_bl_update_status,
};

static int aat2870_bl_probe(struct platform_device *pdev)
{
	struct aat2870_bl_driver_data *aat2870_bl;
	struct backlight_device *bd;
	struct backlight_properties props;
	int ret = 0;

	aat2870_bl = devm_kzalloc(&pdev->dev,
				  sizeof(struct aat2870_bl_driver_data),
				  GFP_KERNEL);
	if (!aat2870_bl) {
		ret = -ENOMEM;
		goto out;
	}

	memset(&props, 0, sizeof(struct backlight_properties));

	props.type = BACKLIGHT_RAW;
	bd = devm_backlight_device_register(&pdev->dev, "aat2870-backlight",
					&pdev->dev, aat2870_bl, &aat2870_bl_ops,
					&props);
	if (IS_ERR(bd)) {
		dev_err(&pdev->dev,
			"Failed allocate memory for backlight device\n");
		ret = PTR_ERR(bd);
		goto out;
	}

	aat2870_bl->pdev = pdev;
	platform_set_drvdata(pdev, aat2870_bl);

	aat2870_bl->bd = bd;

	aat2870_bl->channels = AAT2870_BL_CH_ALL;
	device_property_read_u8(&pdev->dev, "skyworks,channels", &aat2870_bl->channels);

	device_property_read_u32(&pdev->dev, "led-max-microamp", &aat2870_bl->max_current);
	aat2870_bl->max_current = clamp(aat2870_bl->max_current, AAT2870_CURRENT_MIN,
					AAT2870_CURRENT_MAX);
	aat2870_bl->max_current /= AAT2870_CURRENT_STEP;

	bd->props.max_brightness = 255;
	device_property_read_u32(&pdev->dev, "max-brightness", &bd->props.max_brightness);

	aat2870_bl->brightness = 0;
	bd->props.power = BACKLIGHT_POWER_ON;
	bd->props.brightness = bd->props.max_brightness;

	ret = aat2870_bl_update_status(bd);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to initialize\n");
		return ret;
	}

	return 0;

out:
	return ret;
}

static void aat2870_bl_remove(struct platform_device *pdev)
{
	struct aat2870_bl_driver_data *aat2870_bl = platform_get_drvdata(pdev);
	struct backlight_device *bd = aat2870_bl->bd;

	bd->props.power = BACKLIGHT_POWER_OFF;
	bd->props.brightness = 0;
	backlight_update_status(bd);
}

static const struct of_device_id aat2870_bl_match_table[] = {
	{ .compatible = "skyworks,aat2870-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, aat2870_bl_match_table);

static struct platform_driver aat2870_bl_driver = {
	.driver = {
		.name	= "aat2870-backlight",
		.of_match_table = aat2870_bl_match_table,
	},
	.probe		= aat2870_bl_probe,
	.remove		= aat2870_bl_remove,
};

static int __init aat2870_bl_init(void)
{
	return platform_driver_register(&aat2870_bl_driver);
}
subsys_initcall(aat2870_bl_init);

static void __exit aat2870_bl_exit(void)
{
	platform_driver_unregister(&aat2870_bl_driver);
}
module_exit(aat2870_bl_exit);

MODULE_DESCRIPTION("AnalogicTech AAT2870 Backlight");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jin Park <jinyoungp@nvidia.com>");
