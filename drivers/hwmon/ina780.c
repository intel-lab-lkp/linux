// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the Texas Instruments INA780 Digital Power Monitor
 *
 * Datasheet:
 * https://www.ti.com/lit/gpn/ina780a
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define INA780_CONFIG		0x0
#define INA780_ADC_CONFIG	0x1
#define INA780_VBUS		0x5
#define INA780_DIETEMP		0x6
#define INA780_CURRENT		0x7
#define INA780_POWER		0x8
#define INA780_ENERGY		0x9
#define INA780_CHARGE		0xa
#define INA780_DIAG_ALRT	0xb
#define INA780_COL		0xc
#define INA780_CUL		0xd
#define INA780_BOVL		0xe
#define INA780_BUVL		0xf
#define INA780_TEMP_LIMIT	0x10
#define INA780_PWR_LIMIT	0x11
#define INA780_MANUFACTURER_ID	0x3e

#define INA780_DIAG_ALRT_TMPOL		BIT(7)
#define INA780_DIAG_ALRT_CURRENTOL	BIT(6)
#define INA780_DIAG_ALRT_CURRENTUL	BIT(5)
#define INA780_DIAG_ALRT_BUSOL		BIT(4)
#define INA780_DIAG_ALRT_BUSUL		BIT(3)
#define INA780_DIAG_ALRT_POL		BIT(2)

#define INA780_BUS_VOLTAGE_LSB		3125	/* 3.125 mV/lsb */
#define INA780_CURRENT_LSB		2400	/* 2.4 mA/lsb  */
#define INA780_TEMP_LSB			125000	/* 125 mC/lsb */
#define INA780_ENERGY_LSB		7680	/* 7.68 mJ/lsb */
#define INA780_POWER_LSB		480000	/* 480 uW/lsb */
#define INA780_PWR_LIMIT_LSB		(256 * INA780_POWER_LSB)  /* 122.88 mW/lsb */

#define INA780_ID			0x5449

static const struct regmap_config ina780_regmap_config = {
	.max_register = INA780_MANUFACTURER_ID,
	.reg_bits = 8,
	.val_bits = 16,
};

struct ina780_data {
	struct mutex lock;
	struct i2c_client *client;
	struct regmap *regmap;
};

static int ina780_read_reg24(const struct i2c_client *client, u8 reg, u32 *val)
{
	u8 data[3];
	int err;

	err = i2c_smbus_read_i2c_block_data(client, reg, 3, data);
	if (err < 0)
		return err;
	if (err != 3)
		return -EIO;
	*val = (data[0] << 16) | (data[1] << 8) | data[2];

	return 0;
}

static int ina780_read_reg40(struct i2c_client *client, int reg, u64 *val)
{
	u8 data[5];
	u32 low;
	int err;

	err = i2c_smbus_read_i2c_block_data(client, reg, 5, data);
	if (err < 0)
		return err;
	if (err != 5)
		return -EIO;

	low = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];
	*val = ((long long)data[0] << 32) | low;

	return 0;
}

static int ina780_read_in(struct device *dev, u32 attr, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int reg, mask;
	int err;

	switch (attr) {
	case hwmon_in_input:
		reg = INA780_VBUS;
		break;
	case hwmon_in_max:
		reg = INA780_BOVL;
		break;
	case hwmon_in_min:
		reg = INA780_BUVL;
		break;
	case hwmon_in_max_alarm:
		reg = INA780_DIAG_ALRT;
		mask = INA780_DIAG_ALRT_BUSOL;
		break;
	case hwmon_in_min_alarm:
		reg = INA780_DIAG_ALRT;
		mask = INA780_DIAG_ALRT_BUSUL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	switch (attr) {
	case hwmon_in_input:
	case hwmon_in_max:
	case hwmon_in_min:
		err = regmap_read(data->regmap, reg, &regval);
		if (err)
			return err;

		*val = (regval * INA780_BUS_VOLTAGE_LSB) / 1000;
		break;
	case hwmon_in_max_alarm:
	case hwmon_in_min_alarm:
		err = regmap_read(data->regmap, reg, &regval);
		if (err)
			return err;
		*val = !!(regval & mask);
		break;
	}

	return 0;
}

static int ina780_write_in(struct device *dev, u32 attr, long val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int reg;

	switch (attr) {
	case hwmon_in_max:
		reg = INA780_BOVL;
		break;
	case hwmon_in_min:
		reg = INA780_BUVL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	val = clamp_val(val, 0, 102396);
	regval = div_u64(val * 1000ULL, INA780_BUS_VOLTAGE_LSB);

	return regmap_write(data->regmap, reg, regval);
}

static int ina780_read_curr(struct device *dev, u32 attr, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int reg, mask;
	int err;

	switch (attr) {
	case hwmon_curr_input:
		reg = INA780_CURRENT;
		break;
	case hwmon_curr_max:
		reg = INA780_COL;
		break;
	case hwmon_curr_min:
		reg = INA780_CUL;
		break;
	case hwmon_curr_max_alarm:
		reg = INA780_DIAG_ALRT;
		mask = INA780_DIAG_ALRT_CURRENTOL;
		break;
	case hwmon_curr_min_alarm:
		reg = INA780_DIAG_ALRT;
		mask = INA780_DIAG_ALRT_CURRENTUL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	switch (attr) {
	case hwmon_curr_input:
	case hwmon_curr_max:
	case hwmon_curr_min:
		err = regmap_read(data->regmap, reg, &regval);
		if (err)
			return err;
		*val = div_s64((s16)regval * INA780_CURRENT_LSB, 1000);
		break;
	case hwmon_curr_max_alarm:
	case hwmon_curr_min_alarm:
		err = regmap_read(data->regmap, reg, &regval);
		if (err)
			return err;
		*val = !!(regval & mask);
		break;
	}

	return 0;
}

static int ina780_write_curr(struct device *dev, u32 attr, long val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int reg;

	switch (attr) {
	case hwmon_curr_max:
		reg = INA780_COL;
		break;
	case hwmon_curr_min:
		reg = INA780_CUL;
		break;
	default:
		return -EOPNOTSUPP;
	}
	clamp_val(val, -78643, 78640);
	regval = div_s64(val * 1000ULL, INA780_CURRENT_LSB);

	return regmap_write(data->regmap, reg, regval);
}

static int ina780_read_power(struct device *dev, u32 attr, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int err;

	switch (attr) {
	case hwmon_power_input:
		err = ina780_read_reg24(data->client, INA780_POWER, &regval);
		if (err)
			return err;
		*val = div_u64((u64)regval * INA780_POWER_LSB, 1000);
		break;
	case hwmon_power_max:
		err = regmap_read(data->regmap, INA780_PWR_LIMIT, &regval);
		if (err)
			return err;
		*val = div_u64((u64)regval * INA780_PWR_LIMIT_LSB, 1000);
		break;
	case hwmon_power_max_alarm:
		err = regmap_read(data->regmap, INA780_DIAG_ALRT, &regval);
		if (err)
			return err;
		*val = !!(regval & INA780_DIAG_ALRT_POL);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int ina780_write_power(struct device *dev, u32 attr, long val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	int regval;

	switch (attr) {
	case hwmon_power_max:
		val = clamp_val(val, 0, 8052940800);
		regval = div_u64(val * 1000ULL, INA780_PWR_LIMIT_LSB);
		return regmap_write(data->regmap, INA780_PWR_LIMIT, regval);
	default:
		return -EOPNOTSUPP;
	}
}

static int ina780_read_temp(struct device *dev, u32 attr, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	int reg, mask;
	int regval;
	int err;

	switch (attr) {
	case hwmon_temp_input:
		reg = INA780_DIETEMP;
		break;
	case hwmon_temp_max:
		reg = INA780_TEMP_LIMIT;
		break;
	case hwmon_temp_max_alarm:
		reg = INA780_DIAG_ALRT;
		mask = INA780_DIAG_ALRT_TMPOL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_max:
		err = regmap_read(data->regmap, reg, &regval);
		if (err)
			return err;
		*val = div_s64(((s64)((s16)regval) >> 4) * INA780_TEMP_LSB, 1000);
		break;
	case hwmon_temp_max_alarm:
		err = regmap_read(data->regmap, INA780_DIAG_ALRT, &regval);
		*val = !!(regval & INA780_DIAG_ALRT_TMPOL);
		break;
	}

	return 0;
}

static int ina780_write_temp(struct device *dev, u32 attr, long val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	int regval;

	if (attr != hwmon_temp_max)
		return -EOPNOTSUPP;

	val = clamp_val(val, -40000, 150000);
	regval = div_s64(val * 1000, INA780_TEMP_LSB) << 4;

	return regmap_write(data->regmap, INA780_TEMP_LIMIT, regval);
}

static int ina780_read_energy(struct device *dev, u32 attr, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);
	u64 regval;
	int err;

	if (attr != hwmon_energy_input)
		return -EOPNOTSUPP;

	err = ina780_read_reg40(data->client, INA780_ENERGY, &regval);
	if (err)
		return err;

	*val = div_u64(regval * INA780_ENERGY_LSB, 1000);

	return 0;
}

static int ina780_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct ina780_data *data = dev_get_drvdata(dev);

	guard(mutex)(&data->lock);

	switch (type) {
	case hwmon_in:
		return ina780_read_in(dev, attr, val);
	case hwmon_curr:
		return ina780_read_curr(dev, attr, val);
	case hwmon_power:
		return ina780_read_power(dev, attr, val);
	case hwmon_temp:
		return ina780_read_temp(dev, attr, val);
	case hwmon_energy:
		return ina780_read_energy(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int ina780_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	struct ina780_data *data = dev_get_drvdata(dev);

	guard(mutex)(&data->lock);

	switch (type) {
	case hwmon_in:
		return ina780_write_in(dev, attr, val);
	case hwmon_curr:
		return ina780_write_curr(dev, attr, val);
	case hwmon_power:
		return ina780_write_power(dev, attr, val);
	case hwmon_temp:
		return ina780_write_temp(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t ina780_is_visible(const void *drvdata,
				 enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_max_alarm:
		case hwmon_in_min_alarm:
			return 0444;
		case hwmon_in_max:
		case hwmon_in_min:
			return 0644;
		default:
			return 0;
		}
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
		case hwmon_curr_max_alarm:
		case hwmon_curr_min_alarm:
			return 0444;
		case hwmon_curr_max:
		case hwmon_curr_min:
			return 0644;
		default:
			return 0;
		}
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
		case hwmon_power_max_alarm:
			return 0444;
		case hwmon_power_max:
			return 0644;
		default:
			return 0;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_max_alarm:
			return 0444;
		case hwmon_temp_max:
			return 0644;
		default:
			return 0;
		}
	case hwmon_energy:
		switch (attr) {
		case hwmon_energy_input:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const ina238_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM |
			   HWMON_I_MIN | HWMON_I_MIN_ALARM),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT |
			   HWMON_C_MAX | HWMON_C_MAX_ALARM |
			   HWMON_C_MIN | HWMON_C_MIN_ALARM),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_MAX | HWMON_P_MAX_ALARM),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_ALARM),
	HWMON_CHANNEL_INFO(energy,
			   HWMON_E_INPUT),
	NULL
};

static const struct hwmon_ops ina780_hwmon_ops = {
	.is_visible = ina780_is_visible,
	.read = ina780_read,
	.write = ina780_write,
};

static const struct hwmon_chip_info ina780_chip_info = {
	.ops = &ina780_hwmon_ops,
	.info = ina238_info,
};

static int ina780_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ina780_data *data;
	struct device *hwmon_dev;
	unsigned int val;
	int err;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->regmap = devm_regmap_init_i2c(client, &ina780_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(dev, "Failed to allocate register map\n");
		return PTR_ERR(data->regmap);
	}

	err = regmap_read(data->regmap, INA780_MANUFACTURER_ID, &val);
	if (err) {
		dev_err(dev, "Error reading device ID\n");
		return err;
	}

	if (val != INA780_ID)
		dev_warn(dev, "Unexpected device ID %04x\n", val);

	mutex_init(&data->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, data,
							 &ina780_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	/* Temp limit register can go to 255 C but actual max is 150 C*/
	err = ina780_write_temp(hwmon_dev, hwmon_temp_max, 150000);
	if (err)
		return err;

	return 0;
}

static const struct i2c_device_id ina780_id[] = {
	{ "ina780a" },
	{ "ina780b" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ina780_id);

static const struct of_device_id ina780_of_match[] = {
	{ .compatible = "ti,ina780a" },
	{ .compatible = "ti,ina780b" },
	{ }
};
MODULE_DEVICE_TABLE(of, ina780_of_match);

static struct i2c_driver ina780_driver = {
	.driver = {
		.name = "ina780",
		.of_match_table = ina780_of_match,
	},
	.probe = ina780_probe,
	.id_table = ina780_id,
};

module_i2c_driver(ina780_driver);

MODULE_AUTHOR("Chris Packham <chris.packham@alliedtelesis.co.nz>");
MODULE_DESCRIPTION("INA780 driver");
MODULE_LICENSE("GPL");
