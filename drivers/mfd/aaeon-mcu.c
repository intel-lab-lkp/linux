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
#include <linux/mfd/core.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static const struct mfd_cell aaeon_mcu_devs[] = {
	{
		.name = "aaeon-mcu-wdt",
	},
	{
		.name = "aaeon-mcu-gpio",
	},
};

/*
 * Custom regmap bus for the Aaeon MCU I2C protocol.
 *
 * The MCU uses a fixed 3-byte command format [opcode, arg, value] followed
 * by a 1-byte response.  It requires a STOP condition between the command
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
	struct i2c_client *client = context;
	/* data = [opcode, arg, value] as formatted by regmap */
	struct i2c_msg write_msg = {
		.addr  = client->addr,
		.flags = 0,
		.buf   = (u8 *)data,
		.len   = count,
	};
	u8 rsp;
	/* The MCU always sends a response byte after each command; discard it. */
	struct i2c_msg rsp_msg = {
		.addr  = client->addr,
		.flags = I2C_M_RD,
		.buf   = &rsp,
		.len   = 1,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &write_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	ret = i2c_transfer(client->adapter, &rsp_msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
}

static int aaeon_mcu_regmap_read(void *context, const void *reg_buf,
				 size_t reg_size, void *val_buf, size_t val_size)
{
	struct i2c_client *client = context;
	/*
	 * reg_buf holds the 2-byte big-endian register address [opcode, arg].
	 * Append a trailing 0x00 to form the full 3-byte MCU command.
	 */
	u8 cmd[3] = { ((u8 *)reg_buf)[0], ((u8 *)reg_buf)[1], 0x00 };
	struct i2c_msg write_msg = {
		.addr  = client->addr,
		.flags = 0,
		.buf   = cmd,
		.len   = sizeof(cmd),
	};
	struct i2c_msg read_msg = {
		.addr  = client->addr,
		.flags = I2C_M_RD,
		.buf   = val_buf,
		.len   = val_size,
	};
	int ret;

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

static const struct regmap_config aaeon_mcu_regmap_config = {
	.reg_bits          = 16,
	.val_bits          = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type        = REGCACHE_NONE,
};

static int aaeon_mcu_probe(struct i2c_client *client)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&client->dev, &aaeon_mcu_regmap_bus,
				  client, &aaeon_mcu_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
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
		.name = "aaeon_mcu",
		.of_match_table = aaeon_mcu_of_match,
	},
	.probe = aaeon_mcu_probe,
};
module_i2c_driver(aaeon_mcu_driver);

MODULE_DESCRIPTION("Aaeon MCU Driver");
MODULE_AUTHOR("Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>");
MODULE_LICENSE("GPL");
