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

/* Host interface */
#define NCT6694_I2C_MOD		0x03

/* Message Channel*/
/* Command 00h */
#define NCT6694_I2C_CMD0_OFFSET	0x0000	/* OFFSET = SEL|CMD */
#define NCT6694_I2C_CMD0_LEN	0x90

enum i2c_baudrate {
	I2C_BR_25K = 0,
	I2C_BR_50K,
	I2C_BR_100K,
	I2C_BR_200K,
	I2C_BR_400K,
	I2C_BR_800K,
	I2C_BR_1M
};

struct __packed nct6694_i2c_cmd0 {
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
	unsigned char *xmit_buf;
	unsigned char port;
	unsigned char br;
};

static int nct6694_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct nct6694_i2c_data *data = adap->algo_data;
	struct nct6694_i2c_cmd0 *cmd = (struct nct6694_i2c_cmd0 *)data->xmit_buf;
	int ret, i;

	for (i = 0; i < num ; i++) {
		struct i2c_msg *msg_temp = &msgs[i];

		memset(data->xmit_buf, 0, sizeof(struct nct6694_i2c_cmd0));

		if (msg_temp->len > 64)
			return -EPROTO;
		cmd->port = data->port;
		cmd->br = data->br;
		cmd->addr = i2c_8bit_addr_from_msg(msg_temp);
		if (msg_temp->flags & I2C_M_RD) {
			cmd->r_cnt = msg_temp->len;
			ret = nct6694_write_msg(data->nct6694, NCT6694_I2C_MOD,
						NCT6694_I2C_CMD0_OFFSET,
						NCT6694_I2C_CMD0_LEN,
						cmd);
			if (ret < 0)
				return 0;

			memcpy(msg_temp->buf, cmd->read_data, msg_temp->len);
		} else {
			cmd->w_cnt = msg_temp->len;
			memcpy(cmd->write_data, msg_temp->buf, msg_temp->len);
			ret = nct6694_write_msg(data->nct6694, NCT6694_I2C_MOD,
						NCT6694_I2C_CMD0_OFFSET,
						NCT6694_I2C_CMD0_LEN,
						cmd);
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

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->xmit_buf = devm_kcalloc(&pdev->dev, NCT6694_MAX_PACKET_SZ,
				      sizeof(unsigned char), GFP_KERNEL);
	if (!data->xmit_buf)
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
