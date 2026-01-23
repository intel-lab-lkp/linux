// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU driver
 *
 * Copyright (C) 2025 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/aaeon-mcu.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>

#define AAEON_MCU_FW_VERSION	0x76

static struct mfd_cell aaeon_mcu_devs[] = {
	{
		.name = "aaeon-mcu-wdt",
	},
	{
		.name = "aaeon-mcu-gpio",
	},
};

static int aaeon_mcu_read_version(struct device *dev, u8 index, u8 *version)
{
	u8 cmd[3] = { AAEON_MCU_FW_VERSION, index, 0x00 };

	return aaeon_mcu_i2c_xfer(dev, cmd, sizeof(cmd), version, sizeof(*version));
}

static int aaeon_mcu_print_fw_version(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u8 major, minor;
	int ret;

	ret = aaeon_mcu_read_version(dev, 0x00, &major);
	if (ret)
		return ret;

	ret = aaeon_mcu_read_version(dev, 0x01, &minor);
	if (ret)
		return ret;

	dev_info(dev, "firmware version: v%d.%d\n", major, minor);

	return 0;
}

int aaeon_mcu_i2c_xfer(struct device *dev,
		       const u8 *cmd, int cmd_len,
		       u8 *rsp, int rsp_len)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aaeon_mcu_dev *mcu = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&mcu->i2c_lock);

	ret = i2c_master_send(client, cmd, cmd_len);
	if (ret < 0)
		goto unlock;

	ret = i2c_master_recv(client, rsp, rsp_len);
	if (ret < 0)
		goto unlock;

	if (ret != rsp_len) {
		dev_err(dev,
			"i2c recv count error (expected: %d, actual: %d)\n",
			rsp_len, ret);
		ret = -EIO;
		goto unlock;
	}

	ret = 0;

unlock:
	mutex_unlock(&mcu->i2c_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(aaeon_mcu_i2c_xfer);

static int aaeon_mcu_probe(struct i2c_client *client)
{
	struct aaeon_mcu_dev *mcu;
	int ret;

	mcu = devm_kzalloc(&client->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	i2c_set_clientdata(client, mcu);
	mcu->dev = &client->dev;
	mutex_init(&mcu->i2c_lock);

	ret = aaeon_mcu_print_fw_version(client);
	if (ret) {
		dev_err(&client->dev, "unable to read firmware version\n");
		return ret;
	}

	return devm_mfd_add_devices(mcu->dev, PLATFORM_DEVID_NONE, aaeon_mcu_devs,
				    ARRAY_SIZE(aaeon_mcu_devs), NULL, 0, NULL);
}

static const struct of_device_id aaeon_mcu_of_match[] = {
	{ .compatible = "aaeon,srg-imx8pl-mcu" },
	{},
};

MODULE_DEVICE_TABLE(of, aaeon_mcu_of_match);

static struct i2c_driver aaeon_mcu_driver = {
	.driver = {
		.name = "aaeon_mcu",
		.of_match_table = aaeon_mcu_of_match,
	},
	.probe = aaeon_mcu_probe,
};

module_i2c_driver(aaeon_mcu_driver);

MODULE_DESCRIPTION("Aaeon MCU Driver");
MODULE_AUTHOR("Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>");
MODULE_LICENSE("GPL");
