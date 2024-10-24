// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 I2C adapter driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>

/* Host interface */
#define REQUEST_I2C_MOD		0x03

/* Message Channel*/
/* Command 00h */
#define REQUEST_I2C_OFFSET	0x0000	/* OFFSET = SEL|CMD */
#define REQUEST_I2C_LEN		0x90
#define I2C_PORT_IDX		0x00
#define I2C_BR_IDX		0x01
#define I2C_ADDR_IDX		0x02
#define I2C_W_CNT_IDX		0x03
#define I2C_R_CNT_IDX		0x04

#define I2C_RD_IDX		0x50
#define I2C_WR_IDX		0x10

#define DRVNAME "nct6694-i2c"

enum i2c_baudrate {
	I2C_BR_25K = 0,
	I2C_BR_50K,
	I2C_BR_100K,
	I2C_BR_200K,
	I2C_BR_400K,
	I2C_BR_800K,
	I2C_BR_1M
};

struct nct6694_i2c_data {
	struct nct6694 *nct6694;
	struct i2c_adapter adapter;
	unsigned char port;
	unsigned char br;
};

static int nct6694_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct nct6694_i2c_data *data = adap->algo_data;
	int ret, i;

	for (i = 0; i < num ; i++) {
		unsigned char buf[REQUEST_I2C_LEN] = {0};
		struct i2c_msg *msg_temp = &msgs[i];

		if (msg_temp->len > 64)
			return -EPROTO;

		buf[I2C_PORT_IDX] = data->port;
		buf[I2C_BR_IDX] = data->br;
		buf[I2C_ADDR_IDX] = i2c_8bit_addr_from_msg(msg_temp);
		if (msg_temp->flags & I2C_M_RD) {
			buf[I2C_R_CNT_IDX] = msg_temp->len;
			ret = nct6694_write_msg(data->nct6694, REQUEST_I2C_MOD,
						REQUEST_I2C_OFFSET, REQUEST_I2C_LEN,
						buf);
			if (ret < 0)
				return 0;
			memcpy(msg_temp->buf, buf + I2C_RD_IDX, msg_temp->len);
		} else {
			buf[I2C_W_CNT_IDX] = msg_temp->len;
			memcpy(buf + I2C_WR_IDX, msg_temp->buf, msg_temp->len);
			ret = nct6694_write_msg(data->nct6694, REQUEST_I2C_MOD,
						REQUEST_I2C_OFFSET, REQUEST_I2C_LEN,
						buf);
			if (ret < 0)
				return 0;
		}
	}

	return num;
}

static u32 nct6694_func(struct i2c_adapter *adapter)
{
	return (I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL);
}

static const struct i2c_algorithm algorithm = {
	.master_xfer = nct6694_xfer,
	.functionality = nct6694_func,
};

static int nct6694_i2c_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_i2c_data *data;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->nct6694 = nct6694;
	data->port = cell->id;
	data->br = I2C_BR_100K;

	sprintf(data->adapter.name, "NCT6694 I2C Adapter %d", cell->id);
	data->adapter.owner = THIS_MODULE;
	data->adapter.algo = &algorithm;
	data->adapter.dev.parent = &pdev->dev;
	data->adapter.algo_data = data;

	platform_set_drvdata(pdev, data);

	ret = i2c_add_adapter(&data->adapter);
	if (ret) {
		dev_err(&pdev->dev, "%s: Failed to register I2C Adapter: %pe\n",
			__func__, ERR_PTR(ret));
	}

	return ret;
}

static void nct6694_i2c_remove(struct platform_device *pdev)
{
	struct nct6694_i2c_data *data = platform_get_drvdata(pdev);

	i2c_del_adapter(&data->adapter);
}

static struct platform_driver nct6694_i2c_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_i2c_probe,
	.remove		= nct6694_i2c_remove,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_i2c_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_i2c_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_i2c_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-I2C adapter driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
