// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip / Atmel SHA204A (I2C) driver.
 *
 * Copyright (c) 2019 Linaro, Ltd. <ard.biesheuvel@linaro.org>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include "atmel-i2c.h"

static int atmel_sha204a_otp_read(struct i2c_client *client, u16 addr, u8 *otp)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	struct atmel_i2c_cmd cmd;
	int ret;

	ret = atmel_i2c_init_read_otp_cmd(&cmd, addr, &data->timings);
	if (ret < 0) {
		dev_err(&client->dev, "failed, invalid otp address %04X\n",
			addr);
		return ret;
	}

	ret = atmel_i2c_send_receive(client, &cmd);
	if (ret < 0) {
		dev_err(&client->dev, "failed to read otp at %04X\n", addr);
		return ret;
	}

	if (cmd.data[0] == 0xff) {
		dev_err(&client->dev, "failed, device not ready\n");
		return -EIO;
	}

	memcpy(otp, cmd.data+1, 4);

	return ret;
}

static ssize_t otp_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	u16 addr;
	u8 otp[OTP_ZONE_SIZE];
	struct i2c_client *client = to_i2c_client(dev);
	ssize_t len = 0;
	int i, ret;

	for (addr = 0; addr < OTP_ZONE_SIZE / 4; addr++) {
		ret = atmel_sha204a_otp_read(client, addr, otp + addr * 4);
		if (ret < 0) {
			dev_err(dev, "failed to read otp zone\n");
			return ret;
		}
	}

	for (i = 0; i < OTP_ZONE_SIZE; i++)
		len += sysfs_emit_at(buf, len, "%02X", otp[i]);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}
static DEVICE_ATTR_RO(otp);

static struct attribute *atmel_sha204a_attrs[] = {
	&dev_attr_otp.attr,
	NULL
};

static const struct attribute_group atmel_sha204a_groups = {
	.name = "atsha204a",
	.attrs = atmel_sha204a_attrs,
};

static int atmel_sha204a_probe(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv;
	const struct atmel_i2c_of_match_data *data;
	int ret;

	ret = atmel_i2c_probe(client);
	if (ret)
		goto done;

	data = device_get_match_data(&client->dev);
	if (!data) {
		dev_err(&client->dev, "no match data found via OF or ID table\n");
		ret = -ENODEV;
		goto done;
	}

	i2c_priv = i2c_get_clientdata(client);
	i2c_priv->data = data;
	i2c_priv->caps = 0;

	/* add to client list */
	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);
	list_add_tail(&i2c_priv->i2c_client_list_node,
		      &atmel_i2c_mgmt.i2c_client_list);
	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);

	ret = sysfs_create_group(&client->dev.kobj, &atmel_sha204a_groups);
	if (ret) {
		dev_err(&client->dev, "failed to register sysfs entry\n");
		goto err_list_del;
	}

	/* register rng */
	ret = atmel_i2c_register_rng(i2c_priv, &client->dev);
	if (ret) {
		dev_err(&client->dev, "failed to register hw_random\n");
		goto err_list_del;
	}

	goto done;

err_list_del:
	sysfs_remove_group(&client->dev.kobj, &atmel_sha204a_groups);
	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);
	list_del(&i2c_priv->i2c_client_list_node);
	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);

done:
	return ret;
}

static void atmel_sha204a_remove(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);

	devm_hwrng_unregister(&client->dev, &i2c_priv->hwrng);
	atmel_i2c_flush_queue();

	if (i2c_priv->hwrng.priv) {
		kfree((void *)i2c_priv->hwrng.priv);
		i2c_priv->hwrng.priv = 0;
	}

	sysfs_remove_group(&client->dev.kobj, &atmel_sha204a_groups);
}

static const struct atmel_i2c_of_match_data atsha204_match_data = {
	.timings = {
		.max_exec_time_genkey = 43,
		.max_exec_time_random = 50,
		.max_exec_time_read = 4,
		.max_exec_time_write = 42,
	},
	/*
	 * According to review by Bill Cox [1], the ATSHA204 has very low entropy.
	 * [1] https://www.metzdowd.com/pipermail/cryptography/2014-December/023858.html
	 */
	.needs_legacy_hwrng = 1,
};

static const struct atmel_i2c_of_match_data atsha204a_match_data = {
	.timings = {
		.max_exec_time_genkey = 43,
		.max_exec_time_random = 50,
		.max_exec_time_read = 4,
		.max_exec_time_write = 42,
	},
};

static const struct of_device_id atmel_sha204a_dt_ids[] __maybe_unused = {
	{ .compatible = "atmel,atsha204", .data = &atsha204_match_data, },
	{ .compatible = "atmel,atsha204a", .data = &atsha204a_match_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atmel_sha204a_dt_ids);

static const struct i2c_device_id atmel_sha204a_id[] = {
	{ "atsha204" },
	{ "atsha204a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, atmel_sha204a_id);

static struct i2c_driver atmel_sha204a_driver = {
	.probe			= atmel_sha204a_probe,
	.remove			= atmel_sha204a_remove,
	.id_table		= atmel_sha204a_id,

	.driver.name		= "atmel-sha204a",
	.driver.of_match_table	= of_match_ptr(atmel_sha204a_dt_ids),
};

module_i2c_driver(atmel_sha204a_driver);

MODULE_AUTHOR("Ard Biesheuvel <ard.biesheuvel@linaro.org>");
MODULE_DESCRIPTION("Microchip / Atmel SHA204A (I2C) driver");
MODULE_LICENSE("GPL v2");
