// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Microchip/SMSC EMC181x Temperature monitor
 *
 * Copyright (C) 2018-2019 Traverse Technologies
 * Author: Mathew McBride <matt@traverse.com.au>
 *
 * Copyright (C) 2023 Allied Telesis Labs
 * Author: Mark Tomlinson <mark.tomlinson@alliedtelesis.co.nz>
 *
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/of_device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define EMC1812_ID	0x81
#define EMC1813_ID	0x87
#define EMC1814_ID	0x84
#define EMC1815_ID	0x85
#define EMC1833_ID	0x83

#define MICROCHIP_ID	0x54

#define EMC181X_STATUS_REG	0x02
#define EMC181X_CONFIG_REG	0x03
#define EMC181X_DIODE_DATA_BASE	0x60
#define EMC181X_PID_REG		0xfd
#define EMC181X_SMSC_ID_REG	0xfe
#define EMC181X_REVISION_REG	0xff

/* Adjustable address type is 0x7c, 0x5c, 0x4c, 0x6c, 0x1c, 0x3c */
/* Fixed address is either 0x4c or 0x45 */
static const unsigned short emc181x_address_list[] = {
	0x7c, 0x5c, 0x4c, 0x6c, 0x1c, 0x3c, 0x45, I2C_CLIENT_END
};

enum chips {
	emc1812, emc1813,
	emc1814, emc1815,
	emc1833,
};

struct emc181x_data {
	struct i2c_client *i2c;
	enum chips type;
	bool is_extended_range;
};

static int emc181x_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct emc181x_data *data = dev_get_drvdata(dev);
	struct i2c_client *i2c = data->i2c;

	u8 channel_reg = 0;
	s32 channel_deg = 0;
	s32 channel_frac = 0;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;
	if (channel > 4)
		return -EINVAL;

	channel_reg = EMC181X_DIODE_DATA_BASE + (channel * 0x02);
	channel_deg = i2c_smbus_read_byte_data(i2c, channel_reg);
	if (channel_deg < 0)
		return channel_deg;
	if (data->is_extended_range)
		channel_deg -= 64;

	channel_frac = i2c_smbus_read_byte_data(i2c, channel_reg + 0x01);
	if (channel_frac < 0)
		return channel_frac;

	*val = ((channel_deg << 3) | (channel_frac >> 5)) * 125;
	return 0;
}

static umode_t emc181x_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;
	else
		return 0;
}

static const struct hwmon_channel_info *emc1812_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_channel_info *emc1813_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_channel_info *emc1814_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
			   HWMON_T_INPUT),
	NULL
};

static const struct hwmon_channel_info *emc1815_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
			   HWMON_T_INPUT, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops emc181x_ops = {
	.is_visible = emc181x_is_visible,
	.read = emc181x_read,
};

static const struct hwmon_chip_info emc181x_chip_info[] = {
	[emc1812] = {
		.ops = &emc181x_ops,
		.info = emc1812_info,
	},
	[emc1813] = {
		.ops = &emc181x_ops,
		.info = emc1813_info,
	},
	[emc1814] = {
		.ops = &emc181x_ops,
		.info = emc1814_info,
	},
	[emc1815] = {
		.ops = &emc181x_ops,
		.info = emc1815_info,
	},
	[emc1833] = {
		.ops = &emc181x_ops,
		.info = emc1813_info,
	},
};

static const struct i2c_device_id emc181x_i2c_id[] = {
	{ "emc1812", emc1812 },
	{ "emc1813", emc1813 },
	{ "emc1814", emc1814 },
	{ "emc1815", emc1815 },
	{ "emc1833", emc1833 },
	{}
};

MODULE_DEVICE_TABLE(i2c, emc181x_i2c_id);

/* Return 0 if detection is successful, -ENODEV otherwise */
static int emc181x_i2c_detect(struct i2c_client *client,
		       struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int pid, id, rev;
	const char *name;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	/* Determine the chip type */
	id = i2c_smbus_read_byte_data(client, EMC181X_SMSC_ID_REG);
	if (id < 0)
		return id;
	if (id != MICROCHIP_ID)
		return -ENODEV;
	pid = i2c_smbus_read_byte_data(client, EMC181X_PID_REG);
	if (pid < 0)
		return pid;
	rev = i2c_smbus_read_byte_data(client, EMC181X_REVISION_REG);
	if (rev < 0)
		return rev;

	switch (pid) {
	case EMC1812_ID:
		name = emc181x_i2c_id[emc1812].name;
		break;
	case EMC1813_ID:
		name = emc181x_i2c_id[emc1813].name;
		break;
	case EMC1814_ID:
		name = emc181x_i2c_id[emc1814].name;
		break;
	case EMC1815_ID:
		name = emc181x_i2c_id[emc1815].name;
		break;
	case EMC1833_ID:
		name = emc181x_i2c_id[emc1833].name;
		break;
	default:
		return -ENODEV;
	}
	strscpy(info->type, name, I2C_NAME_SIZE);

	dev_dbg(&client->dev,
		"Detected device %s at 0x%02x with COMPANY: 0x%02x and PID: 0x%02x REV: 0x%02x\n",
		name, client->addr, id, pid, rev);

	return 0;
}

static int emc181x_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct emc181x_data *data;
	s32 regval;
	u8 config;

	data = devm_kzalloc(dev, sizeof(struct emc181x_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->i2c = client;
	if (dev_fwnode(dev)) {
		data->type = (enum chips)device_get_match_data(dev);
		data->is_extended_range = device_property_present(dev, "emc181x,extended-range");
	} else
		data->type = i2c_match_id(emc181x_i2c_id, client)->driver_data;

	regval = i2c_smbus_read_byte_data(client, EMC181X_CONFIG_REG);
	if (regval < 0) {
		dev_dbg(dev, "Failed to read config reg %d", regval);
		return regval;
	}

	/* By default, extended range is disabled in the EMC181X,
	 * if enabled we need to set this in the CONFIG register.
	 */
	config = regval & ~0x04;
	if (data->is_extended_range)
		config |= 0x04;

	dev_dbg(dev, "EMC181X setting CONFIG to %d\n", config);
	if (config != regval)
		i2c_smbus_write_byte_data(client, EMC181X_CONFIG_REG, config);

	hwmon_dev = devm_hwmon_device_register_with_info(dev,
			client->name,
			data,
			&emc181x_chip_info[data->type],
			NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id __maybe_unused emc181x_of_match[] = {
	{
		.compatible = "microchip,emc1812",
		.data = (void *)emc1812
	},
	{
		.compatible = "microchip,emc1813",
		.data = (void *)emc1813
	},
	{
		.compatible = "microchip,emc1814",
		.data = (void *)emc1814
	},
	{
		.compatible = "microchip,emc1815",
		.data = (void *)emc1815
	},
	{
		.compatible = "microchip,emc1833",
		.data = (void *)emc1833
	},
	{ },
};
MODULE_DEVICE_TABLE(of, emc181x_of_match);

static struct i2c_driver emc181x_i2c_driver = {
	.driver = {
		.name = "emc181x",
		.of_match_table = of_match_ptr(emc181x_of_match),
	},
	.probe = emc181x_i2c_probe,
	.id_table = emc181x_i2c_id,
	.detect = emc181x_i2c_detect,
	.address_list = emc181x_address_list,
};

module_i2c_driver(emc181x_i2c_driver);

MODULE_DESCRIPTION("EMC181X Sensor Driver");
MODULE_AUTHOR("Mark Tomlinson <mark.tomlinson@alliedtelesis.co.nz>");
MODULE_LICENSE("GPL");
