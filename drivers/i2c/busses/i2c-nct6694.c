// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 I2C adapter driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/*
 * USB command module type for NCT6694 I2C controller.
 * This defines the module type used for communication with the NCT6694
 * I2C controller over the USB interface.
 */
#define NCT6694_I2C_MOD		0x03

/* Command 00h - I2C Deliver */
#define NCT6694_I2C_DELIVER	0x00
#define NCT6694_I2C_DELIVER_SEL	0x00

enum i2c_baudrate {
	I2C_BR_25K = 0,
	I2C_BR_50K,
	I2C_BR_100K,
	I2C_BR_200K,
	I2C_BR_400K,
	I2C_BR_800K,
	I2C_BR_1M
};

struct __packed nct6694_i2c_deliver {
	u8 port;
	u8 br;
	u8 addr;
	u8 w_cnt;
	u8 r_cnt;
	u8 rsv[11];
	u8 write_data[0x40];
	u8 read_data[0x40];
};

struct nct6694_i2c_data {
	struct nct6694 *nct6694;
	struct i2c_adapter adapter;
	struct nct6694_i2c_deliver *deliver;
	unsigned char port;
	unsigned char br;
};

static int nct6694_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct nct6694_i2c_data *data = adap->algo_data;
	struct nct6694_i2c_deliver *deliver = data->deliver;
	struct nct6694_cmd_header cmd_hd = {
		.mod = NCT6694_I2C_MOD,
		.cmd = NCT6694_I2C_DELIVER,
		.sel = NCT6694_I2C_DELIVER_SEL,
		.len = cpu_to_le16(sizeof(*deliver))
	};
	int ret, i;

	for (i = 0; i < num ; i++) {
		struct i2c_msg *msg_temp = &msgs[i];

		memset(deliver, 0, sizeof(*deliver));

		if (msg_temp->len > 64)
			return -EPROTO;

		deliver->port = data->port;
		deliver->br = data->br;
		deliver->addr = i2c_8bit_addr_from_msg(msg_temp);
		if (msg_temp->flags & I2C_M_RD) {
			deliver->r_cnt = msg_temp->len;
			ret = nct6694_write_msg(data->nct6694, &cmd_hd, deliver);
			if (ret < 0)
				return ret;

			memcpy(msg_temp->buf, deliver->read_data, msg_temp->len);
		} else {
			deliver->w_cnt = msg_temp->len;
			memcpy(deliver->write_data, msg_temp->buf, msg_temp->len);
			ret = nct6694_write_msg(data->nct6694, &cmd_hd, deliver);
			if (ret < 0)
				return ret;
		}
	}

	return num;
}

static u32 nct6694_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
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

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->deliver = devm_kzalloc(&pdev->dev, sizeof(struct nct6694_i2c_deliver),
				     GFP_KERNEL);
	if (!data->deliver)
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

	return i2c_add_adapter(&data->adapter);
}

static void nct6694_i2c_remove(struct platform_device *pdev)
{
	struct nct6694_i2c_data *data = platform_get_drvdata(pdev);

	i2c_del_adapter(&data->adapter);
}

static struct platform_driver nct6694_i2c_driver = {
	.driver = {
		.name	= "nct6694-i2c",
	},
	.probe		= nct6694_i2c_probe,
	.remove		= nct6694_i2c_remove,
};

module_platform_driver(nct6694_i2c_driver);

MODULE_DESCRIPTION("USB-I2C adapter driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:nct6694-i2c");
