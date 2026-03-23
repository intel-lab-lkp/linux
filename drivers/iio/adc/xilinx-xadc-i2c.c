// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx XADC I2C Interface Driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * This driver implements I2C interface support for Xilinx System Management
 * Wizard IP on UltraScale+ devices. It uses the 32-bit DRP (Dynamic
 * Reconfiguration Port) packet format as per Xilinx PG185 specification.
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <linux/iio/iio.h>

#include "xilinx-xadc.h"

#define XADC_I2C_READ_DATA_SIZE		2
#define XADC_I2C_WRITE_DATA_SIZE	4	/* 32-bit DRP packet */
#define XADC_I2C_INSTR_READ		BIT(2)
#define XADC_I2C_INSTR_WRITE		BIT(3)

#define XADC_I2C_DRP_DATA0_MASK		GENMASK(7, 0)
#define XADC_I2C_DRP_DATA1_MASK		GENMASK(15, 8)
#define XADC_I2C_DRP_ADDR_MASK		GENMASK(7, 0)

#define XADC_INPUT_MODE_BITS		16

struct xadc_i2c {
	struct xadc xadc;
	struct i2c_client *client;
	bool hw_initialized;
	unsigned int conf0;
	unsigned int bipolar_mask;
};

static int xadc_i2c_read_transaction(struct xadc *xadc, unsigned int reg, u16 *val)
{
	struct xadc_i2c *xadc_i2c = container_of(xadc, struct xadc_i2c, xadc);
	char write_buffer[XADC_I2C_WRITE_DATA_SIZE] = { 0 };
	struct i2c_client *client = xadc_i2c->client;
	char read_buffer[XADC_I2C_READ_DATA_SIZE];
	int ret;

	write_buffer[2] = FIELD_GET(XADC_I2C_DRP_ADDR_MASK, reg);
	write_buffer[3] = XADC_I2C_INSTR_READ;

	ret = i2c_master_send(client, write_buffer, XADC_I2C_WRITE_DATA_SIZE);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(client, read_buffer, XADC_I2C_READ_DATA_SIZE);
	if (ret < 0)
		return ret;

	*val = FIELD_PREP(XADC_I2C_DRP_DATA0_MASK, read_buffer[0]) |
	       FIELD_PREP(XADC_I2C_DRP_DATA1_MASK, read_buffer[1]);

	return 0;
}

static int xadc_i2c_write_transaction(struct xadc *xadc, unsigned int reg, u16 val)
{
	struct xadc_i2c *xadc_i2c = container_of(xadc, struct xadc_i2c, xadc);
	struct i2c_client *client = xadc_i2c->client;
	char write_buffer[XADC_I2C_WRITE_DATA_SIZE];
	int ret;

	write_buffer[0] = FIELD_GET(XADC_I2C_DRP_DATA0_MASK, val);
	write_buffer[1] = FIELD_GET(XADC_I2C_DRP_DATA1_MASK, val);
	write_buffer[2] = FIELD_GET(XADC_I2C_DRP_ADDR_MASK, reg);
	write_buffer[3] = XADC_I2C_INSTR_WRITE;

	ret = i2c_master_send(client, write_buffer, XADC_I2C_WRITE_DATA_SIZE);
	if (ret < 0)
		return ret;

	return 0;
}

static int xadc_hardware_init(struct xadc *xadc)
{
	struct xadc_i2c *xadc_i2c = container_of(xadc, struct xadc_i2c, xadc);
	int ret;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(xadc->threshold); i++) {
		ret = xadc_i2c_read_transaction(xadc, XADC_REG_THRESHOLD(i),
						&xadc->threshold[i]);
		if (ret)
			return ret;
	}

	ret = xadc_i2c_write_transaction(xadc, XADC_REG_CONF0, xadc_i2c->conf0);
	if (ret)
		return ret;

	ret = xadc_i2c_write_transaction(xadc, XADC_REG_INPUT_MODE(0),
					 xadc_i2c->bipolar_mask);
	if (ret)
		return ret;

	ret = xadc_i2c_write_transaction(xadc, XADC_REG_INPUT_MODE(1),
					 xadc_i2c->bipolar_mask >> XADC_INPUT_MODE_BITS);
	if (ret)
		return ret;

	xadc_i2c->hw_initialized = true;

	return 0;
}

static int xadc_i2c_read_reg(struct xadc *xadc, unsigned int reg, u16 *val)
{
	struct xadc_i2c *xadc_i2c = container_of(xadc, struct xadc_i2c, xadc);

	if (!xadc_i2c->hw_initialized) {
		int ret;

		ret = xadc_hardware_init(xadc);
		if (ret)
			return ret;
	}

	return xadc_i2c_read_transaction(xadc, reg, val);
}

static int xadc_i2c_write_reg(struct xadc *xadc, unsigned int reg, u16 val)
{
	struct xadc_i2c *xadc_i2c = container_of(xadc, struct xadc_i2c, xadc);

	if (!xadc_i2c->hw_initialized) {
		int ret;

		ret = xadc_hardware_init(xadc);
		if (ret)
			return ret;
	}

	return xadc_i2c_write_transaction(xadc, reg, val);
}

static const struct xadc_ops xadc_system_mgmt_wiz_i2c_ops = {
	.read = xadc_i2c_read_reg,
	.write = xadc_i2c_write_reg,
	.setup_channels = xadc_parse_dt,
	.type = XADC_TYPE_US_I2C,
	.temp_scale = 509314,
	.temp_offset = 280231,
};

static int xadc_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	unsigned int conf0, bipolar_mask;
	const struct xadc_ops *ops;
	struct iio_dev *indio_dev;
	struct xadc_i2c *xadc_i2c;
	struct xadc *xadc;
	int ret;

	indio_dev = xadc_device_setup(dev, sizeof(*xadc_i2c), &ops);
	if (IS_ERR(indio_dev))
		return PTR_ERR(indio_dev);

	xadc_i2c = iio_priv(indio_dev);
	xadc_i2c->client = client;
	xadc = &xadc_i2c->xadc;
	xadc->clk = NULL;
	xadc->ops = ops;
	mutex_init(&xadc->mutex);
	spin_lock_init(&xadc->lock);

	ret = xadc_device_configure(dev, indio_dev, 0, &conf0, &bipolar_mask);
	if (ret) {
		dev_err(dev, "Failed to setup the device: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, indio_dev);
	xadc_i2c->conf0 = conf0;
	xadc_i2c->bipolar_mask = bipolar_mask;
	xadc_i2c->hw_initialized = false;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id xadc_i2c_of_match_table[] = {
	{
		.compatible = "xlnx,system-management-wiz-1.3",
		.data = &xadc_system_mgmt_wiz_i2c_ops,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, xadc_i2c_of_match_table);

static struct i2c_driver xadc_i2c_driver = {
	.probe = xadc_i2c_probe,
	.driver = {
		.name = "xadc-i2c",
		.of_match_table = xadc_i2c_of_match_table,
	},
};
module_i2c_driver(xadc_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sai Krishna Potthuri <sai.krishna.potthuri@amd.com>");
MODULE_DESCRIPTION("Xilinx XADC I2C Interface Driver");
