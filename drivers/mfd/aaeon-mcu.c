// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU driver
 *
 * Copyright (C) 2026 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/aaeon-mcu.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct aaeon_mcu {
	struct i2c_client *client;
	u8 *cmd;      /* DMA-safe 3-byte write buffer [opcode, arg, value] */
	u8 *response; /* DMA-safe 1-byte read buffer for MCU acknowledgment */
};

static const struct mfd_cell aaeon_mcu_devs[] = {
	MFD_CELL_BASIC("aaeon-mcu-wdt", NULL, NULL, 0, 0),
	MFD_CELL_BASIC("aaeon-mcu-gpio", NULL, NULL, 0, 0),
};

/* Number of bytes in a MCU command: [opcode, arg, value] */
#define AAEON_MCU_CMD_LEN      3

/*
 * Custom regmap bus for the Aaeon MCU I2C protocol.
 *
 * The MCU uses a fixed 3-byte command format [opcode, arg, value] followed
 * by a 1-byte response. It requires a STOP condition between the command
 * write and the response read, so two separate i2c_transfer() calls are
 * issued.  The regmap lock serialises concurrent accesses from the GPIO
 * and watchdog child drivers.
 *
 * Register addresses are encoded as a 16-bit big-endian value where the
 * high byte is the opcode and the low byte is the argument, matching the
 * wire layout produced by regmap for reg_bits=16.
 */

static int aaeon_mcu_regmap_write(void *context, const void *data, size_t count)
{
	struct aaeon_mcu *mcu = context;
	struct i2c_client *client = mcu->client;
	struct i2c_msg write_msg;
	/* The MCU always sends a response byte after each command; discard it. */
	struct i2c_msg response_msg;
	int ret;

	memcpy(mcu->cmd, data, count);

	write_msg.addr  = client->addr;
	write_msg.flags = I2C_M_DMA_SAFE;
	write_msg.buf   = mcu->cmd;
	write_msg.len   = count;

	response_msg.addr  = client->addr;
	response_msg.flags = I2C_M_RD | I2C_M_DMA_SAFE;
	response_msg.buf   = mcu->response;
	response_msg.len   = 1;

	ret = i2c_transfer(client->adapter, &write_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	ret = i2c_transfer(client->adapter, &response_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
}

static int aaeon_mcu_regmap_read(void *context, const void *reg_buf,
				 size_t reg_size, void *val_buf, size_t val_size)
{
	struct aaeon_mcu *mcu = context;
	struct i2c_client *client = mcu->client;
	struct i2c_msg write_msg;
	struct i2c_msg read_msg;
	int ret;

	/*
	 * reg_buf holds the 2-byte big-endian register address [opcode, arg].
	 * Append a trailing 0x00 to form the full 3-byte MCU command.
	 */
	mcu->cmd[0] = ((u8 *)reg_buf)[0];
	mcu->cmd[1] = ((u8 *)reg_buf)[1];
	mcu->cmd[2] = 0x00;

	write_msg.addr  = client->addr;
	write_msg.flags = I2C_M_DMA_SAFE;
	write_msg.buf   = mcu->cmd;
	write_msg.len   = AAEON_MCU_CMD_LEN;

	read_msg.addr  = client->addr;
	read_msg.flags = I2C_M_RD | I2C_M_DMA_SAFE;
	read_msg.buf   = val_buf;
	read_msg.len   = val_size;

	ret = i2c_transfer(client->adapter, &write_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	ret = i2c_transfer(client->adapter, &read_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
}

static const struct regmap_bus aaeon_mcu_regmap_bus = {
	.write = aaeon_mcu_regmap_write,
	.read  = aaeon_mcu_regmap_read,
};

static bool aaeon_mcu_volatile_reg(struct device *dev, unsigned int reg)
{
	/*
	 * GPIO input registers are driven by external signals and can change
	 * at any time without CPU involvement, always read from hardware.
	 *
	 * The watchdog status register reflects hardware state and can change
	 * autonomously.
	 *
	 * All other registers are written by the driver and their values are
	 * stable, so they can be safely cached.
	 */
	if ((reg >> 8) == AAEON_MCU_READ_GPIO_OPCODE)
		return true;
	if (reg == AAEON_MCU_REG(AAEON_MCU_CONTROL_WDT_OPCODE, 0x02))
		return true;
	return false;
}

static const struct regmap_config aaeon_mcu_regmap_config = {
	.reg_bits          = 16,
	.val_bits          = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.max_register      = AAEON_MCU_MAX_REGISTER,
	.volatile_reg      = aaeon_mcu_volatile_reg,
	.cache_type        = REGCACHE_MAPLE,
};

static int aaeon_mcu_probe(struct i2c_client *client)
{
	struct aaeon_mcu *ddata;
	struct regmap *regmap;

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->client = client;

	ddata->cmd = devm_kzalloc(&client->dev, AAEON_MCU_CMD_LEN * sizeof(*ddata->cmd),
				   GFP_KERNEL);
	if (!ddata->cmd)
		return -ENOMEM;

	ddata->response = devm_kzalloc(&client->dev, sizeof(*ddata->response), GFP_KERNEL);
	if (!ddata->response)
		return -ENOMEM;

	regmap = devm_regmap_init(&client->dev, &aaeon_mcu_regmap_bus,
				  ddata, &aaeon_mcu_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "failed to initialize regmap\n");

	return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				    aaeon_mcu_devs, ARRAY_SIZE(aaeon_mcu_devs),
				    NULL, 0, NULL);
}

static const struct of_device_id aaeon_mcu_of_match[] = {
	{ .compatible = "aaeon,srg-imx8p-mcu" },
	{},
};
MODULE_DEVICE_TABLE(of, aaeon_mcu_of_match);

static struct i2c_driver aaeon_mcu_driver = {
	.driver = {
		.name = "aaeon-mcu",
		.of_match_table = aaeon_mcu_of_match,
	},
	.probe = aaeon_mcu_probe,
};
module_i2c_driver(aaeon_mcu_driver);

MODULE_DESCRIPTION("Aaeon MCU Driver");
MODULE_AUTHOR("Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>");
MODULE_LICENSE("GPL");
