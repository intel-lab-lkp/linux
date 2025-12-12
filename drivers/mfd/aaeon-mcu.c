// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU MFD driver
 *
 * Copyright (C) 2025 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/aaeon-mcu.h>

#define AAEON_MCU_GET_FW_VERSION 0x76

static struct mfd_cell aaeon_mcu_devs[] = {
	{
		.name = "aaeon-mcu-wdt",
		.of_compatible = "aaeon,srg-imx8pl-wdt",
	},
	{
		.name = "aaeon-mcu-gpio",
		.of_compatible = "aaeon,srg-imx8pl-gpio",
	},
};

static int aaeon_mcu_print_fw_version(struct i2c_client *client)
{
	u8 cmd[3], version[2];
	int ret;

	/* Major version number */
	cmd[0] = AAEON_MCU_GET_FW_VERSION;
	cmd[1] = 0x00;
	cmd[2] = 0x00;

	ret = aaeon_mcu_i2c_xfer(client, cmd, 3, &version[0], 1);
	if (ret < 0)
		return ret;

	/* Minor version number */
	cmd[0] = AAEON_MCU_GET_FW_VERSION;
	cmd[1] = 0x01;
	/* cmd[2] = 0x00; */

	ret = aaeon_mcu_i2c_xfer(client, cmd, 3, &version[1], 1);
	if (ret < 0)
		return ret;

	dev_info(&client->dev, "firmware version: v%d.%d\n",
		 version[0], version[1]);

	return 0;
}

static int aaeon_mcu_probe(struct i2c_client *client)
{
	struct aaeon_mcu_dev *mcu;
	int ret;

	mcu = devm_kzalloc(&client->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	i2c_set_clientdata(client, mcu);
	mcu->dev = &client->dev;
	mcu->i2c_client = client;
	mutex_init(&mcu->i2c_lock);

	ret = aaeon_mcu_print_fw_version(client);
	if (ret) {
		dev_err(&client->dev, "unable to read firmware version\n");
		return ret;
	}

	return devm_mfd_add_devices(mcu->dev, PLATFORM_DEVID_NONE, aaeon_mcu_devs,
				    ARRAY_SIZE(aaeon_mcu_devs), NULL, 0, NULL);
}

int aaeon_mcu_i2c_xfer(struct i2c_client *client,
		       const u8 *cmd, int cmd_len,
		       u8 *rsp, int rsp_len)
{
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
		dev_err(&client->dev,
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

MODULE_DESCRIPTION("Aaeon MCU MFD Driver");
MODULE_AUTHOR("Jérémie Dautheribes");
MODULE_LICENSE("GPL");
