// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsi.c - AMD SBTSI I2C core driver. Probes the SBTSI device over I2C
 *         and publishes an auxiliary device on the auxiliary bus.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/misc/tsi.h>
#include <linux/slab.h>

#define SBTSI_REG_CONFIG		0x03 /* RO */

/*
 * Bit for reporting value with temperature measurement range.
 * bit == 0: Use default temperature range (0C to 255.875C).
 * bit == 1: Use extended temperature range (-49C to +206.875C).
 */
#define SBTSI_CONFIG_EXT_RANGE_SHIFT	2

/*
 * ReadOrder bit specifies the reading order of integer and decimal part of
 * CPU temperature for atomic reads. If bit == 0, reading integer part triggers
 * latching of the decimal part, so integer part should be read first.
 */
#define SBTSI_CONFIG_READ_ORDER_SHIFT	5

static void sbtsi_adev_release(struct device *dev)
{
	kfree(to_auxiliary_dev(dev));
}

static void sbtsi_unregister_hwmon_adev(void *_adev)
{
	struct auxiliary_device *adev = _adev;

	auxiliary_device_delete(adev);
	auxiliary_device_uninit(adev);
}

/*
 * Create and publish an auxiliary device. The hwmon driver in
 * drivers/hwmon/sbtsi_temp.c binds to this device.
 *
 * @dev:      I2C device (parent of the auxiliary device)
 * @dev_addr: I2C address — used as the auxiliary device instance ID so that
 *            each socket gets a unique name.
 */
static int sbtsi_create_hwmon_adev(struct device *dev, u8 dev_addr)
{
	struct auxiliary_device *adev;
	int ret;

	adev = kzalloc_obj(*adev);
	if (!adev)
		return -ENOMEM;

	adev->name = AMD_SBTSI_AUX_HWMON;
	adev->id = dev_addr;
	adev->dev.parent = dev;
	adev->dev.release = sbtsi_adev_release;

	ret = auxiliary_device_init(adev);
	if (ret) {
		kfree(adev);
		return ret;
	}

	ret = __auxiliary_device_add(adev, AMD_SBTSI_ADEV);
	if (ret) {
		auxiliary_device_uninit(adev);
		return ret;
	}

	return devm_add_action_or_reset(dev, sbtsi_unregister_hwmon_adev, adev);
}

static int sbtsi_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sbtsi_data *data;
	int err;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	err = i2c_smbus_read_byte_data(data->client, SBTSI_REG_CONFIG);
	if (err < 0)
		return err;
	data->ext_range_mode = FIELD_GET(BIT(SBTSI_CONFIG_EXT_RANGE_SHIFT), err);
	data->read_order = FIELD_GET(BIT(SBTSI_CONFIG_READ_ORDER_SHIFT), err);

	dev_set_drvdata(dev, data);
	return sbtsi_create_hwmon_adev(dev, client->addr);
}

static const struct i2c_device_id sbtsi_id[] = {
	{"sbtsi"},
	{}
};
MODULE_DEVICE_TABLE(i2c, sbtsi_id);

static const struct of_device_id __maybe_unused sbtsi_of_match[] = {
	{
		.compatible = "amd,sbtsi",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, sbtsi_of_match);

static struct i2c_driver sbtsi_driver = {
	.driver = {
		.name = "sbtsi-i2c",
		.of_match_table = of_match_ptr(sbtsi_of_match),
	},
	.probe = sbtsi_i2c_probe,
	.id_table = sbtsi_id,
};

module_i2c_driver(sbtsi_driver);

MODULE_DESCRIPTION("AMD SB-TSI I2C core driver");
MODULE_LICENSE("GPL");
