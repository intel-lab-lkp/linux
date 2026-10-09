// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for the TDK FS1412 uPOL regulator
 *
 * Copyright 2026 Nexthop Systems.
 */

#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pmbus.h>
#include <linux/string.h>

#include "pmbus.h"

#define FS1412_MFR_ID		"KDT"
#define FS1412_MFR_ID_LEN	(sizeof(FS1412_MFR_ID) - 1)

/* VOUT_MODE: bit 7 selects relative VOUT limits, bits [4:0] the exponent. */
#define FS1412_VOUT_MODE_RELATIVE	BIT(7)
#define FS1412_VOUT_MODE_EXP_MASK	0x1f
#define FS1412_VOUT_EXP_SHIFT		8	/* exponent -8: 1/256 V per LSB */

/*
 * VOUT_MODE reads 0x98: LINEAR mode with exponent -8 and bit 7 set, which
 * makes the VOUT limit commands relative to VOUT_COMMAND. The PMBus core
 * treats bits [7:5] as the mode, so report the mode and exponent with bit
 * 7 cleared and convert the relative limits in the word accessors below.
 */
static int fs1412_read_byte_data(struct i2c_client *client, int page, int reg)
{
	int ret;

	if (reg != PMBUS_VOUT_MODE)
		return -ENODATA;

	ret = pmbus_read_byte_data(client, page, reg);
	if (ret < 0 || ret == 0xff)
		return ret;

	return ret & ~FS1412_VOUT_MODE_RELATIVE;
}

static bool fs1412_vout_limit(int reg)
{
	switch (reg) {
	case PMBUS_VOUT_OV_FAULT_LIMIT:
	case PMBUS_VOUT_OV_WARN_LIMIT:
	case PMBUS_VOUT_UV_WARN_LIMIT:
	case PMBUS_VOUT_UV_FAULT_LIMIT:
		return true;
	default:
		return false;
	}
}

/*
 * The VOUT limits hold the limit as a fraction of VOUT_COMMAND in the same
 * 1/256 units, so 0x0133 on a 0.6 V command is a 120% limit. Report them
 * as absolute voltages: limit = fraction * VOUT_COMMAND.
 */
static int fs1412_read_word_data(struct i2c_client *client, int page,
				 int phase, int reg)
{
	int rel, cmd;

	/*
	 * OT_WARN_LIMIT is not in the command table. The register answers,
	 * but the part has no warning threshold and reports OT_WARNING at
	 * room temperature, so keep the core from exposing either.
	 */
	if (reg == PMBUS_OT_WARN_LIMIT)
		return -ENXIO;
	if (!fs1412_vout_limit(reg))
		return -ENODATA;

	rel = pmbus_read_word_data(client, page, phase, reg);
	if (rel < 0)
		return rel;

	cmd = pmbus_read_word_data(client, page, 0xff, PMBUS_VOUT_COMMAND);
	if (cmd < 0)
		return cmd;

	return min_t(u32, DIV_ROUND_CLOSEST((u32)rel * cmd,
					     1 << FS1412_VOUT_EXP_SHIFT), 0xffff);
}

static int fs1412_write_word_data(struct i2c_client *client, int page,
				  int reg, u16 word)
{
	int cmd;

	if (reg == PMBUS_OT_WARN_LIMIT)
		return -ENXIO;
	if (!fs1412_vout_limit(reg))
		return -ENODATA;

	cmd = pmbus_read_word_data(client, page, 0xff, PMBUS_VOUT_COMMAND);
	if (cmd < 0)
		return cmd;
	if (!cmd)
		return -EINVAL;

	word = clamp_val(DIV_ROUND_CLOSEST(word << FS1412_VOUT_EXP_SHIFT, cmd),
			 0, 0xffff);

	return pmbus_write_word_data(client, page, reg, word);
}

static struct pmbus_driver_info fs1412_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT |
		   PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
		   PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP,
	.read_byte_data = fs1412_read_byte_data,
	.read_word_data = fs1412_read_word_data,
	.write_word_data = fs1412_write_word_data,
};

static int fs1412_probe(struct i2c_client *client)
{
	char buf[I2C_SMBUS_BLOCK_MAX + 1] = { 0 };
	struct device *dev = &client->dev;
	int ret;

	ret = i2c_smbus_read_block_data(client, PMBUS_MFR_ID, buf);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read PMBUS_MFR_ID\n");

	if (ret != FS1412_MFR_ID_LEN ||
	    strncmp(buf, FS1412_MFR_ID, FS1412_MFR_ID_LEN))
		return dev_err_probe(dev, -ENODEV,
				     "Unsupported Manufacturer ID '%s'\n", buf);

	/* The relative limit conversion assumes the 1/256 V exponent. */
	ret = i2c_smbus_read_byte_data(client, PMBUS_VOUT_MODE);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read VOUT_MODE\n");
	if ((ret & FS1412_VOUT_MODE_EXP_MASK) != 0x18)
		return dev_err_probe(dev, -ENODEV,
				     "Unsupported VOUT_MODE 0x%02x\n", ret);

	return pmbus_do_probe(client, &fs1412_info);
}

static const struct i2c_device_id fs1412_id[] = {
	{ .name = "fs1412" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fs1412_id);

static const struct of_device_id fs1412_of_match[] = {
	{ .compatible = "tdk,fs1412" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs1412_of_match);

static struct i2c_driver fs1412_driver = {
	.driver = {
		.name = "fs1412",
		.of_match_table = fs1412_of_match,
	},
	.probe = fs1412_probe,
	.id_table = fs1412_id,
};

module_i2c_driver(fs1412_driver);

MODULE_AUTHOR("Abdurrahman Hussain <abdurrahman@nexthop.ai>");
MODULE_DESCRIPTION("PMBus driver for the TDK FS1412 uPOL regulator");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
