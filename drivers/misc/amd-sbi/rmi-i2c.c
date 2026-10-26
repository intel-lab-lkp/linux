// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rmi-i2c.c - Side band RMI over I2C support for AMD out
 *             of band management
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include "rmi-core.h"

#define REV_TWO_BYTE_ADDR	0x21

static int sbrmi_enable_alert(struct sbrmi_data *data)
{
	int ctrl, ret;

	/*
	 * Enable the SB-RMI Software alert status
	 * by writing 0 to bit 4 of Control register(0x1)
	 */
	ret = regmap_read(data->regmap, SBRMI_CTRL, &ctrl);
	if (ret < 0)
		return ret;

	if (ctrl & 0x10) {
		ctrl &= ~0x10;
		return regmap_write(data->regmap, SBRMI_CTRL, ctrl);
	}

	return 0;
}

static int sbrmi_get_max_pwr_limit(struct sbrmi_data *data)
{
	struct apml_mbox_msg msg = { 0 };
	int ret;

	msg.cmd = SBRMI_READ_PKG_MAX_PWR_LIMIT;
	ret = rmi_mailbox_xfer(data, &msg);
	if (ret < 0)
		return ret;
	data->pwr_limit_max = msg.mb_in_out;

	return ret;
}

static void sbrmi_misc_unregister(void *arg)
{
	struct sbrmi_data *data = arg;

	misc_deregister(&data->sbrmi_misc_dev);

	mutex_lock(&data->lock);
	data->detached = true;
	mutex_unlock(&data->lock);
}

static void sbrmi_driver_unref(void *arg)
{
	struct sbrmi_data *data = arg;

	kref_put(&data->kref, sbrmi_data_release);
}

static int sbrmi_common_probe(struct device *dev, struct regmap *regmap, uint8_t address)
{
	struct sbrmi_data *data;
	int ret;

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;

	data->regmap = regmap;
	mutex_init(&data->lock);
	kref_init(&data->kref);

	ret = devm_add_action_or_reset(dev, sbrmi_driver_unref, data);
	if (ret)
		return ret;

	/* Enable alert for SB-RMI sequence */
	ret = sbrmi_enable_alert(data);
	if (ret < 0)
		return ret;

	/* Cache maximum power limit */
	ret = sbrmi_get_max_pwr_limit(data);
	if (ret < 0)
		return ret;

	data->dev_static_addr = address;

	dev_set_drvdata(dev, data);

	ret = create_hwmon_sensor_device(dev, data);
	if (ret < 0)
		return ret;

	ret = create_misc_rmi_device(data, dev);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, sbrmi_misc_unregister, data);
}

static struct regmap_config sbrmi_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static struct regmap_config sbrmi_regmap_config_ext = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int sbrmi_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	int rev, ret;

	regmap = devm_regmap_init_i2c(client, &sbrmi_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = regmap_read(regmap, SBRMI_REV, &rev);
	if (ret)
		return ret;

	/*
	 * For Turin and newer platforms, revision is 0x21 or later. This is
	 * to identify the two byte register address size. However, one
	 * byte transaction can be successful.
	 * Verify if revision is 0x21 or later, if yes, switch to 2 byte
	 * address size.
	 * Continuously using 1 byte address for revision 0x21 or later can lead
	 * to bus corruption.
	 */
	if (rev >= REV_TWO_BYTE_ADDR) {
		regmap = devm_regmap_init_i2c(client, &sbrmi_regmap_config_ext);
		if (IS_ERR(regmap))
			return PTR_ERR(regmap);
	}
	return sbrmi_common_probe(dev, regmap, client->addr);
}

static const struct i2c_device_id sbrmi_id[] = {
	{ .name = "sbrmi-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sbrmi_id);

static const struct of_device_id __maybe_unused sbrmi_of_match[] = {
	{
		.compatible = "amd,sbrmi",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, sbrmi_of_match);

static struct i2c_driver sbrmi_driver = {
	.driver = {
		.name = "sbrmi-i2c",
		.of_match_table = of_match_ptr(sbrmi_of_match),
	},
	.probe = sbrmi_i2c_probe,
	.id_table = sbrmi_id,
};

static int sbrmi_i3c_probe(struct i3c_device *i3cdev)
{
	struct device *dev = i3cdev_to_dev(i3cdev);
	struct regmap *regmap;
	int rev, ret;

	/*
	 * AMD OOB devices are distinguished by their Instance ID.
	 * For SBRMI, the Instance ID is 1. Since the device ID match
	 * does not account for the Instance ID, the following check
	 * ensures that only the SBRMI device is probed, excluding
	 * other OOB devices.
	 */
	if (I3C_PID_INSTANCE_ID(i3cdev->desc->info.pid) != 1)
		return -ENXIO;

	regmap = devm_regmap_init_i3c(i3cdev, &sbrmi_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = regmap_read(regmap, SBRMI_REV, &rev);
	if (ret)
		return ret;

	/*
	 * For Turin and newer platforms, revision is 0x21 or later. This is
	 * to identify the two byte register address size. However, one
	 * byte transaction can be successful.
	 * Verify if revision is 0x21 or later, if yes, switch to 2 byte
	 * address size.
	 * Continuously using 1 byte address for revision 0x21 or later can lead
	 * to bus corruption.
	 */
	if (rev >= REV_TWO_BYTE_ADDR) {
		regmap = devm_regmap_init_i3c(i3cdev, &sbrmi_regmap_config_ext);
		if (IS_ERR(regmap))
			return PTR_ERR(regmap);
	}

	/*
	 * AMD APML I3C devices support static address.
	 * If static address is defined, dynamic address is same as static address.
	 * In case static address is not defined, I3C master controller defined
	 * dynamic address is used.
	 */
	return sbrmi_common_probe(dev, regmap, i3cdev->desc->info.dyn_addr);
}

static const struct i3c_device_id sbrmi_i3c_id[] = {
	/* PID for AMD SBRMI device */
	I3C_DEVICE_EXTRA_INFO(0x112, 0x0, 0x2, NULL),
	I3C_DEVICE_EXTRA_INFO(0x0, 0x0, 0x118, NULL), /* Socket:0, Venice */
	I3C_DEVICE_EXTRA_INFO(0x0, 0x100, 0x118, NULL), /* Socket:1, Venice */
	I3C_DEVICE_EXTRA_INFO(0x112, 0x0, 0x119, NULL), /* Socket:0, Venice */
	I3C_DEVICE_EXTRA_INFO(0x112, 0x100, 0x119, NULL), /* Socket:1, Venice */
	{ }
};
MODULE_DEVICE_TABLE(i3c, sbrmi_i3c_id);

static struct i3c_driver sbrmi_i3c_driver = {
	.driver = {
		.name = "sbrmi-i3c",
	},
	.probe = sbrmi_i3c_probe,
	.id_table = sbrmi_i3c_id,
};

module_i3c_i2c_driver(sbrmi_i3c_driver, &sbrmi_driver);

MODULE_AUTHOR("Akshay Gupta <akshay.gupta@amd.com>");
MODULE_AUTHOR("Naveen Krishna Chatradhi <naveenkrishna.chatradhi@amd.com>");
MODULE_DESCRIPTION("Hwmon driver for AMD SB-RMI emulated sensor");
MODULE_LICENSE("GPL");
