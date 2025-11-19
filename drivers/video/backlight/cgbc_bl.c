// SPDX-License-Identifier: GPL-2.0-only
/*
 * Congatec Board Controller (CGBC) Backlight Driver
 *
 * This driver provides backlight control for LCD displays connected to
 * Congatec boards via the CGBC (Congatec Board Controller). It integrates
 * with the Linux backlight subsystem and communicates with hardware through
 * the cgbc-core module.
 *
 * Copyright (C) 2025 Novatron Oy
 *
 * Author: Petri Karhula <petri.karhula@novatron.fi>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>

#include <linux/mfd/cgbc.h>

#define CGBC_BL_DRIVER_VERSION     "0.0.1"

#define BLT_PWM_DUTY_MASK          0x7F
#define BLT_PWM_INVERTED_MASK      0x80

/* CGBC command for PWM brightness control*/
#define CGBC_CMD_BLT0_PWM          0x75

#define CGBC_BL_MAX_BRIGHTNESS     100

/**
 * CGBC backlight driver data
 * @dev: Pointer to the platform device
 * @bl_dev: Pointer to the backlight device
 * @cgbc: Pointer to the parent CGBC device data
 * @current_brightness: Current brightness level (0-100)
 */
struct cgbc_bl_data {
	struct device *dev;
	struct backlight_device *bl_dev;
	struct cgbc_device_data *cgbc;
	unsigned int current_brightness;
};

/**
 * Read current PWM settings from hardware
 * @bl_data: Backlight driver data
 *
 * Reads the current PWM duty cycle percentage (= brightness level)
 * from the board controller.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cgbc_bl_read_pwm_settings(struct cgbc_bl_data *bl_data)
{
	u8 cmd_buf[4] = { CGBC_CMD_BLT0_PWM, 0, 0, 0 };
	u8 reply_buf[3];
	int ret;

	ret = cgbc_command(bl_data->cgbc, cmd_buf, sizeof(cmd_buf), reply_buf,
			   sizeof(reply_buf), NULL);

	if (ret < 0) {
		dev_err(bl_data->dev, "Failed to read PWM settings: %d\n", ret);
		return ret;
	}

	/*
	 * Only return PWM duty factor percentage,
	 * ignore polarity inversion bit (bit 7)
	 */
	bl_data->current_brightness = reply_buf[0] & BLT_PWM_DUTY_MASK;

	dev_dbg(bl_data->dev, "Current PWM duty=%u\n", bl_data->current_brightness);

	return 0;
}

/**
 * Set backlight brightness
 * @bl_data: Backlight driver data
 * @brightness: Brightness level (0-100)
 *
 * Sets the backlight brightness by configuring the PWM duty cycle.
 * Preserves the current polarity and frequency settings.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cgbc_bl_set_brightness(struct cgbc_bl_data *bl_data, u8 brightness)
{
	u8 cmd_buf[4] = { CGBC_CMD_BLT0_PWM, 0, 0, 0 };
	u8 reply_buf[3];
	int ret;

	/* Read the current values */
	ret = cgbc_command(bl_data->cgbc, cmd_buf, sizeof(cmd_buf), reply_buf,
			   sizeof(reply_buf), NULL);

	if (ret < 0) {
		dev_err(bl_data->dev, "Failed to read PWM settings: %d\n", ret);
		return ret;
	}

	/*
	 * Prepare command buffer for writing new settings. Only 2nd byte is changed
	 * to set new brightness (PWM duty cycle %). Other balues (polarity, frequency)
	 * are preserved from the read values.
	 */
	cmd_buf[1] = (reply_buf[0] & BLT_PWM_INVERTED_MASK) |
		     (BLT_PWM_DUTY_MASK & brightness);
	cmd_buf[2] = reply_buf[1];
	cmd_buf[3] = reply_buf[2];

	ret = cgbc_command(bl_data->cgbc, cmd_buf, sizeof(cmd_buf), reply_buf,
			   sizeof(reply_buf), NULL);

	if (ret < 0) {
		dev_err(bl_data->dev, "Failed to set brightness: %d\n", ret);
		return ret;
	}

	bl_data->current_brightness = reply_buf[0] & BLT_PWM_DUTY_MASK;

	/* Verify the setting was applied correctly */
	if (bl_data->current_brightness != brightness) {
		dev_err(bl_data->dev,
			"Brightness setting verification failed\n");
		return -EIO;
	}

	dev_dbg(bl_data->dev, "Set brightness to %u\n", brightness);

	return 0;
}

/**
 * Backlight update callback
 * @bl: Backlight device
 *
 * Called by the backlight subsystem when brightness needs to be updated.
 * Changes the brightness level on the hardware
 * if requested value differs from the current setting.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cgbc_bl_update_status(struct backlight_device *bl)
{
	struct cgbc_bl_data *bl_data = bl_get_data(bl);
	u8 brightness;
	int ret;

	brightness = bl->props.brightness;

	if (brightness != bl_data->current_brightness) {
		ret = cgbc_bl_set_brightness(bl_data, brightness);

		if (ret < 0) {
			dev_err(bl_data->dev, "Failed to set brightness: %d\n",
			       ret);
			return ret;
		}
	}

	return 0;
}

/**
 * Get current backlight brightness
 * @bl: Backlight device
 *
 * Returns the current brightness level by reading from hardware.
 *
 * Return: Current brightness level (0-100), or negative error code
 */
static int cgbc_bl_get_brightness(struct backlight_device *bl)
{
	struct cgbc_bl_data *bl_data = bl_get_data(bl);
	int ret;

	/* Read current PWM brightness settings */
	ret = cgbc_bl_read_pwm_settings(bl_data);

	if (ret < 0) {
		dev_err(bl_data->dev, "Failed to read PWM settings: %d\n", ret);
		return ret;
	}

	return bl_data->current_brightness;
}

/* Backlight device operations */
static const struct backlight_ops cgbc_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = cgbc_bl_update_status,
	.get_brightness = cgbc_bl_get_brightness,
};

/**
 * Probe function for CGBC backlight driver
 * @pdev: Platform device
 *
 * Initializes the CGBC backlight driver and registers it with the
 * Linux backlight subsystem.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cgbc_bl_probe(struct platform_device *pdev)
{
	struct cgbc_device_data *cgbc = dev_get_drvdata(pdev->dev.parent);
	struct cgbc_bl_data *bl_data;
	struct backlight_properties props;
	struct backlight_device *bl_dev;
	int ret;

	bl_data = devm_kzalloc(&pdev->dev, sizeof(*bl_data), GFP_KERNEL);

	if (!bl_data)
		return -ENOMEM;

	bl_data->dev = &pdev->dev;
	bl_data->cgbc = cgbc;

	ret = cgbc_bl_read_pwm_settings(bl_data);

	if (ret) {
		dev_err(&pdev->dev, "Failed to read initial PWM settings: %d\n",
			ret);
		return ret;
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = CGBC_BL_MAX_BRIGHTNESS;
	props.brightness = bl_data->current_brightness;

	bl_dev = devm_backlight_device_register(&pdev->dev, "cgbc-backlight",
						&pdev->dev, bl_data,
						&cgbc_bl_ops, &props);

	if (IS_ERR(bl_dev)) {
		dev_err(&pdev->dev, "Failed to register backlight device\n");
		return PTR_ERR(bl_dev);
	}

	bl_data->bl_dev = bl_dev;
	platform_set_drvdata(pdev, bl_data);

	dev_info(&pdev->dev,
		 "CGBC backlight driver registered (brightness=%u)\n",
		 bl_data->current_brightness);

	return 0;
}

/**
 * Remove function for CGBC backlight driver
 * @pdev: Platform device
 *
 * The Linux device-managed resource framework (devres) does the cleanup.
 * No explicit cleanup is needed here.
 */
static void cgbc_bl_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "CGBC backlight driver removed\n");
}

static struct platform_driver cgbc_bl_driver = {
	.driver = {
		.name = "cgbc-backlight",
	},
	.probe = cgbc_bl_probe,
	.remove = cgbc_bl_remove,
};

module_platform_driver(cgbc_bl_driver);

MODULE_AUTHOR("Petri Karhula <petri.karhula@novatron.fi>");
MODULE_DESCRIPTION("Congatec Board Controller (CGBC) Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(CGBC_BL_DRIVER_VERSION);
MODULE_ALIAS("platform:cgbc-backlight");
