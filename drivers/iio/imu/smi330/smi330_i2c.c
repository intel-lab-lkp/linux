// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (c) 2025 Robert Bosch GmbH.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include "smi330.h"

#define I2C_XFER_MAX_RETRY 10
#define I2C_WRITE_DELAY_TIME 10

#define NUM_DUMMY_BYTES 2
#define SMI330_I2C_MAX_RX_BUFFER_SIZE (NUM_DUMMY_BYTES + 32)

struct smi330_i2c_priv {
	struct i2c_client *i2c;
	u8 rx_buffer[SMI330_I2C_MAX_RX_BUFFER_SIZE] __aligned(IIO_DMA_MINALIGN);
};

static int smi330_regmap_i2c_read(void *context, const void *reg_buf,
				  size_t reg_size, void *val_buf,
				  size_t val_size)
{
	struct smi330_i2c_priv *priv = context;
	int ret, retry;

	/*
	 * SMI330 I2C read frame:
	 * <Slave address[6:0], RnW> <x, Register address[6:0]>
	 * <Slave address[6:0], RnW> <Dummy[7:0]> <Dummy[7:0]> <Data_0[7:0]> <Data_1[15:8]>...
	 *                                                     <Data_N[7:0]> <Data_N[15:8]>
	 * Remark: Slave address is not considered part of the frame in the following definitions
	 */
	struct i2c_msg msgs[] = {
		{
			.addr = priv->i2c->addr,
			.flags = priv->i2c->flags,
			.len = reg_size,
			.buf = (u8 *)reg_buf,
		},
		{
			.addr = priv->i2c->addr,
			.flags = priv->i2c->flags | I2C_M_RD,
			.len = NUM_DUMMY_BYTES + val_size,
			.buf = priv->rx_buffer,
		},
	};

	for (retry = 0; retry < I2C_XFER_MAX_RETRY; retry++) {
		ret = i2c_transfer(priv->i2c->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret > 0)
			break;

		usleep_range(I2C_WRITE_DELAY_TIME * 1000,
			     I2C_WRITE_DELAY_TIME * 1000);
	}

	if (retry >= I2C_XFER_MAX_RETRY) {
		dev_err(&priv->i2c->adapter->dev, "Xfer error");
		return -EIO;
	}

	memcpy(val_buf, priv->rx_buffer + NUM_DUMMY_BYTES, val_size);

	return 0;
}

static int smi330_regmap_i2c_write(void *context, const void *data,
				   size_t count)
{
	struct smi330_i2c_priv *priv = context;
	u8 reg;

	/*
	 * SMI330 I2C write frame:
	 * <Slave address[6:0], RnW> <x, Register address[6:0]> <Data_0[7:0]> <Data_1[15:8]>...
	 *                                                      <Data_N[7:0]> <Data_N[15:8]>
	 * Remark: Slave address is not considered part of the frame in the following definitions
	 */
	reg = *(u8 *)data;
	return i2c_smbus_write_i2c_block_data(priv->i2c, reg,
					      count - sizeof(u8),
					      data + sizeof(u8));
}

static const struct regmap_bus smi330_regmap_bus = {
	.read = smi330_regmap_i2c_read,
	.write = smi330_regmap_i2c_write,
};

static const struct regmap_config smi330_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int smi330_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct smi330_i2c_priv *priv;
	struct regmap *regmap;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2c = i2c;
	regmap = devm_regmap_init(dev, &smi330_regmap_bus, priv,
				  &smi330_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to initialize I2C Regmap\n");

	return smi330_core_probe(dev, regmap);
}

static const struct i2c_device_id smi330_i2c_device_id[] = {
	{ .name = "smi330" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, smi330_i2c_device_id);

static const struct of_device_id smi330_of_match[] = {
	{ .compatible = "bosch,smi330" },
	{ }
};
MODULE_DEVICE_TABLE(of, smi330_of_match);

static struct i2c_driver smi330_i2c_driver = {
	.probe = smi330_i2c_probe,
	.id_table = smi330_i2c_device_id,
	.driver = { .of_match_table = smi330_of_match,
		    .name = "smi330_i2c",
	},
};
module_i2c_driver(smi330_i2c_driver);
