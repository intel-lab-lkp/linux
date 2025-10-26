// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for ST Microelectronics TSC1641 I2C power monitor
 *
 * 60 V, 16-bit high-precision power monitor with I2C and MIPI I3C interface
 * Datasheet: https://www.st.com/resource/en/datasheet/tsc1641.pdf
 *
 * Copyright (C) 2025 Igor Reznichenko <igor@reznichenko.net>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

/* I2C registers */
#define TSC1641_CONFIG		0x00
#define TSC1641_SHUNT_VOLTAGE	0x01
#define TSC1641_LOAD_VOLTAGE	0x02
#define TSC1641_POWER		0x03
#define TSC1641_CURRENT		0x04
#define TSC1641_TEMP		0x05
#define TSC1641_MASK		0x06
#define TSC1641_FLAG		0x07
#define TSC1641_RSHUNT		0x08 /* Shunt resistance */
#define TSC1641_SOL		0x09
#define TSC1641_SUL		0x0A
#define TSC1641_LOL		0x0B
#define TSC1641_LUL		0x0C
#define TSC1641_POL		0x0D
#define TSC1641_TOL		0x0E
#define TSC1641_MANUF_ID	0xFE /* 0x0006 */
#define TSC1641_DIE_ID		0xFF /* 0x1000 */
#define TSC1641_MAX_REG		0xFF

#define TSC1641_RSHUNT_DEFAULT	1000   /* 1mOhm */
#define TSC1641_CONFIG_DEFAULT	0x003F /* Enable temperature sensor */
#define TSC1641_MASK_DEFAULT	0xFC00 /* Unmask all alerts */

/* Bit mask for conversion time in the configuration register */
#define TSC1641_CONV_TIME_MASK	GENMASK(7, 4)

#define TSC1641_CONV_TIME_DEFAULT	1024
#define TSC1641_MIN_UPDATE_INTERVAL	1024

/* LSB value of different registers */
#define TSC1641_VLOAD_LSB_MVOLT		2
#define TSC1641_POWER_LSB_UWATT		25000
#define TSC1641_VSHUNT_LSB_NVOLT	2500 /* Use nanovolts to make it integer */
#define TSC1641_RSHUNT_LSB_UOHM		10
#define TSC1641_TEMP_LSB_MDEGC		500

/* Limits based on datasheet */
#define TSC1641_RSHUNT_MIN_UOHM		100
#define TSC1641_RSHUNT_MAX_UOHM		655350
#define TSC1641_VLOAD_MAX_MVOLT		60000
#define TSC1641_CURRENT_MIN_MAMP	(-819175)
#define TSC1641_CURRENT_MAX_MAMP	819175
#define TSC1641_TEMP_MIN_MDEGC		(-20000)
#define TSC1641_TEMP_MAX_MDEGC		145000
#define TSC1641_POWER_MAX_UWATT		1600000000

#define TSC1641_ALERT_POL_MASK		BIT(1)
#define TSC1641_ALERT_LATCH_EN_MASK	BIT(0)

/* Flags indicating alerts in TSC1641_FLAG register*/
#define TSC1641_SHUNT_OV_FLAG		BIT(6)
#define TSC1641_SHUNT_UV_FLAG		BIT(5)
#define TSC1641_LOAD_OV_FLAG		BIT(4)
#define TSC1641_LOAD_UV_FLAG		BIT(3)
#define TSC1641_POWER_OVER_FLAG		BIT(2)
#define TSC1641_TEMP_OVER_FLAG		BIT(1)

static bool tsc1641_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TSC1641_CONFIG:
	case TSC1641_MASK:
	case TSC1641_RSHUNT:
	case TSC1641_SOL:
	case TSC1641_SUL:
	case TSC1641_LOL:
	case TSC1641_LUL:
	case TSC1641_POL:
	case TSC1641_TOL:
		return true;
	default:
		return false;
	}
}

static bool tsc1641_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TSC1641_SHUNT_VOLTAGE:
	case TSC1641_LOAD_VOLTAGE:
	case TSC1641_POWER:
	case TSC1641_CURRENT:
	case TSC1641_TEMP:
	case TSC1641_FLAG:
	case TSC1641_MANUF_ID:
	case TSC1641_DIE_ID:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tsc1641_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.use_single_write = true,
	.use_single_read = true,
	.max_register = TSC1641_MAX_REG,
	.cache_type = REGCACHE_MAPLE,
	.volatile_reg = tsc1641_volatile_reg,
	.writeable_reg = tsc1641_writeable_reg,
};

struct tsc1641_data {
	long rshunt_uohm;
	long current_lsb_ua;
	struct regmap *regmap;
	struct i2c_client *client;
};

/*
 * Upper limit due to chip 16-bit shunt register, lower limit to
 * prevent current and power registers overflow
 */
static inline int tsc1641_validate_shunt(u32 val)
{
	if (val < TSC1641_RSHUNT_MIN_UOHM || val > TSC1641_RSHUNT_MAX_UOHM)
		return -EINVAL;
	return 0;
}

static int tsc1641_set_shunt(struct tsc1641_data *data, u32 val)
{
	struct regmap *regmap = data->regmap;
	long rshunt_reg;

	if (tsc1641_validate_shunt(val) < 0)
		return -EINVAL;

	data->rshunt_uohm = val;
	data->current_lsb_ua = DIV_ROUND_CLOSEST(TSC1641_VSHUNT_LSB_NVOLT * 1000,
						 data->rshunt_uohm);
	/* RSHUNT register LSB is 10uOhm so need to divide further*/
	rshunt_reg = DIV_ROUND_CLOSEST(data->rshunt_uohm, TSC1641_RSHUNT_LSB_UOHM);
	return regmap_write(regmap, TSC1641_RSHUNT, clamp_val(rshunt_reg, 0, USHRT_MAX));
}

/*
 * Conversion times in uS, value in CONFIG[CT3:CT0] corresponds to index in this array
 * See "Table 14. CT3 to CT0: conversion time" in:
 * https://www.st.com/resource/en/datasheet/tsc1641.pdf
 */
static const int tsc1641_conv_times[] = { 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

static int tsc1641_reg_to_upd_interval(u16 config)
{
	int idx = FIELD_GET(TSC1641_CONV_TIME_MASK, config);

	idx = clamp_val(idx, 0, ARRAY_SIZE(tsc1641_conv_times) - 1);
	int conv_time = tsc1641_conv_times[idx];

	/* Don't support sub-millisecond update interval as it's not supported in hwmon */
	conv_time = max(conv_time, TSC1641_MIN_UPDATE_INTERVAL);
	/* Return nearest value in milliseconds */
	return DIV_ROUND_CLOSEST(conv_time, 1000);
}

static u16 tsc1641_upd_interval_to_reg(long interval)
{
	/* Supported interval is 1ms - 33ms */
	interval = clamp_val(interval, 1, 33);

	int conv = interval * 1000;
	int conv_bits = find_closest(conv, tsc1641_conv_times,
				     ARRAY_SIZE(tsc1641_conv_times));

	return FIELD_PREP(TSC1641_CONV_TIME_MASK, conv_bits);
}

static int tsc1641_chip_write(struct device *dev, u32 attr, long val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_chip_update_interval:
		return regmap_update_bits(data->regmap, TSC1641_CONFIG,
					  TSC1641_CONV_TIME_MASK,
					  tsc1641_upd_interval_to_reg(val));
	default:
		return -EOPNOTSUPP;
	}
}

static int tsc1641_chip_read(struct device *dev, u32 attr, long *val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	u32 regval;
	int ret;

	switch (attr) {
	case hwmon_chip_update_interval:
		ret = regmap_read(data->regmap, TSC1641_CONFIG, &regval);
		if (ret)
			return ret;

		*val = tsc1641_reg_to_upd_interval(regval);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int tsc1641_alert_read(struct regmap *regmap, u32 flag, long *val)
{
	unsigned int regval;
	int ret;

	ret = regmap_read_bypassed(regmap, TSC1641_FLAG, &regval);
	if (ret)
		return ret;

	*val = !!(regval & flag);
	return 0;
}

static int tsc1641_in_read(struct device *dev, u32 attr, long *val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	int ret, reg;

	switch (attr) {
	case hwmon_in_input:
		reg = TSC1641_LOAD_VOLTAGE;
		break;
	case hwmon_in_lcrit:
		reg = TSC1641_LUL;
		break;
	case hwmon_in_crit:
		reg = TSC1641_LOL;
		break;
	case hwmon_in_lcrit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_LOAD_UV_FLAG, val);
	case hwmon_in_crit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_LOAD_OV_FLAG, val);
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_read(regmap, reg, &regval);
	if (ret)
		return ret;

	*val = regval * TSC1641_VLOAD_LSB_MVOLT;
	return 0;
}

static int tsc1641_curr_read(struct device *dev, u32 attr, long *val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	int regval;
	int ret, reg;

	/* Current limits are the shunt under/over voltage limits */
	switch (attr) {
	case hwmon_curr_input:
		reg = TSC1641_CURRENT;
		break;
	case hwmon_curr_lcrit:
		reg = TSC1641_SUL;
		break;
	case hwmon_curr_crit:
		reg = TSC1641_SOL;
		break;
	case hwmon_curr_lcrit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_SHUNT_UV_FLAG, val);
	case hwmon_curr_crit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_SHUNT_OV_FLAG, val);
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_read(regmap, reg, &regval);
	if (ret)
		return ret;

	/* Current in milliamps */
	*val = DIV_ROUND_CLOSEST((s16)regval * data->current_lsb_ua, 1000);
	return 0;
}

static int tsc1641_power_read(struct device *dev, u32 attr, long *val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	int ret, reg;

	switch (attr) {
	case hwmon_power_input:
		reg = TSC1641_POWER;
		break;
	case hwmon_power_crit:
		reg = TSC1641_POL;
		break;
	case hwmon_power_crit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_POWER_OVER_FLAG, val);
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_read(regmap, reg, &regval);
	if (ret)
		return ret;

	*val = regval * TSC1641_POWER_LSB_UWATT;
	return 0;
}

static int tsc1641_temp_read(struct device *dev, u32 attr, long *val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	int ret, reg;

	switch (attr) {
	case hwmon_temp_input:
		reg = TSC1641_TEMP;
		break;
	case hwmon_temp_crit:
		reg = TSC1641_TOL;
		break;
	case hwmon_temp_crit_alarm:
		return tsc1641_alert_read(regmap, TSC1641_TEMP_OVER_FLAG, val);
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_read(regmap, reg, &regval);
	if (ret)
		return ret;

	*val = (s16)regval * TSC1641_TEMP_LSB_MDEGC;
	return 0;
}

static int tsc1641_in_write(struct device *dev, u32 attr, long val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	int reg;

	switch (attr) {
	case hwmon_in_lcrit:
		reg = TSC1641_LUL;
		break;
	case hwmon_in_crit:
		reg = TSC1641_LOL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	val = clamp_val(val, 0, TSC1641_VLOAD_MAX_MVOLT);
	regval = DIV_ROUND_CLOSEST(val, TSC1641_VLOAD_LSB_MVOLT);

	return regmap_write(regmap, reg, clamp_val(regval, 0, USHRT_MAX));
}

static int tsc1641_curr_write(struct device *dev, u32 attr, long val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	int reg, regval;

	switch (attr) {
	case hwmon_curr_lcrit:
		reg = TSC1641_SUL;
		break;
	case hwmon_curr_crit:
		reg = TSC1641_SOL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Clamp to max 16-bit represantable current at min Rshunt */
	val = clamp_val(val, TSC1641_CURRENT_MIN_MAMP, TSC1641_CURRENT_MAX_MAMP);
	/* Convert val in milliamps to voltage */
	regval = DIV_ROUND_CLOSEST(val * data->rshunt_uohm, TSC1641_VSHUNT_LSB_NVOLT);

	return regmap_write(regmap, reg, clamp_val(regval, SHRT_MIN, SHRT_MAX));
}

static int tsc1641_power_write(struct device *dev, u32 attr, long val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	unsigned int regval;

	switch (attr) {
	case hwmon_power_crit:
		val = clamp_val(val, 0, TSC1641_POWER_MAX_UWATT);
		regval = DIV_ROUND_CLOSEST(val, TSC1641_POWER_LSB_UWATT);
		return regmap_write(regmap, TSC1641_POL, clamp_val(regval, 0, USHRT_MAX));
	default:
		return -EOPNOTSUPP;
	}
}

static int tsc1641_temp_write(struct device *dev, u32 attr, long val)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	int regval;

	switch (attr) {
	case hwmon_temp_crit:
		val = clamp_val(val, TSC1641_TEMP_MIN_MDEGC, TSC1641_TEMP_MAX_MDEGC);
		regval = DIV_ROUND_CLOSEST(val, TSC1641_TEMP_LSB_MDEGC);
		return regmap_write(regmap, TSC1641_TOL, clamp_val(regval, SHRT_MIN, SHRT_MAX));
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t tsc1641_is_visible(const void *data, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			return 0444;
		case hwmon_in_lcrit:
		case hwmon_in_crit:
			return 0644;
		case hwmon_in_lcrit_alarm:
		case hwmon_in_crit_alarm:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			return 0444;
		case hwmon_curr_lcrit:
		case hwmon_curr_crit:
			return 0644;
		case hwmon_curr_lcrit_alarm:
		case hwmon_curr_crit_alarm:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
			return 0444;
		case hwmon_power_crit:
			return 0644;
		case hwmon_power_crit_alarm:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			return 0444;
		case hwmon_temp_crit:
			return 0644;
		case hwmon_temp_crit_alarm:
			return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static int tsc1641_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_chip:
		return tsc1641_chip_read(dev, attr, val);
	case hwmon_in:
		return tsc1641_in_read(dev, attr, val);
	case hwmon_curr:
		return tsc1641_curr_read(dev, attr, val);
	case hwmon_power:
		return tsc1641_power_read(dev, attr, val);
	case hwmon_temp:
		return tsc1641_temp_read(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int tsc1641_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_chip:
		return tsc1641_chip_write(dev, attr, val);
	case hwmon_in:
		return tsc1641_in_write(dev, attr, val);
	case hwmon_curr:
		return tsc1641_curr_write(dev, attr, val);
	case hwmon_power:
		return tsc1641_power_write(dev, attr, val);
	case hwmon_temp:
		return tsc1641_temp_write(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const tsc1641_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_CRIT | HWMON_I_CRIT_ALARM |
			   HWMON_I_LCRIT | HWMON_I_LCRIT_ALARM),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_CRIT | HWMON_C_CRIT_ALARM |
			   HWMON_C_LCRIT | HWMON_C_LCRIT_ALARM),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_CRIT | HWMON_P_CRIT_ALARM),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_CRIT | HWMON_T_CRIT_ALARM),
	NULL
};

static ssize_t shunt_resistor_show(struct device *dev,
				   struct device_attribute *da, char *buf)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%li\n", data->rshunt_uohm);
}

static ssize_t shunt_resistor_store(struct device *dev,
				    struct device_attribute *da,
				    const char *buf, size_t count)
{
	struct tsc1641_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val > U32_MAX)
		return -EINVAL;

	ret = tsc1641_set_shunt(data, (u32)val);
	if (ret < 0)
		return ret;
	return count;
}

static const struct hwmon_ops tsc1641_hwmon_ops = {
	.is_visible = tsc1641_is_visible,
	.read = tsc1641_read,
	.write = tsc1641_write,
};

static const struct hwmon_chip_info tsc1641_chip_info = {
	.ops = &tsc1641_hwmon_ops,
	.info = tsc1641_info,
};

static DEVICE_ATTR_RW(shunt_resistor);

/* Shunt resistor value is exposed via sysfs attribute */
static struct attribute *tsc1641_attrs[] = {
	&dev_attr_shunt_resistor.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tsc1641);

static int tsc1641_init(struct device *dev, struct tsc1641_data *data)
{
	struct regmap *regmap = data->regmap;
	bool active_high;
	u32 shunt;
	int ret;

	if (device_property_read_u32(dev, "shunt-resistor-micro-ohms", &shunt) < 0)
		shunt = TSC1641_RSHUNT_DEFAULT;

	if (tsc1641_validate_shunt(shunt) < 0) {
		dev_err(dev, "invalid shunt resistor value %u\n", shunt);
		return -EINVAL;
	}

	ret = tsc1641_set_shunt(data, shunt);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, TSC1641_CONFIG, TSC1641_CONFIG_DEFAULT);
	if (ret < 0)
		return ret;

	active_high = device_property_read_bool(dev, "st,alert-polarity-active-high");

	return regmap_write(regmap, TSC1641_MASK, TSC1641_MASK_DEFAULT |
			    FIELD_PREP(TSC1641_ALERT_POL_MASK, active_high));
}

static int tsc1641_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tsc1641_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;

	data->regmap = devm_regmap_init_i2c(client, &tsc1641_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(data->regmap);
	}

	ret = tsc1641_init(dev, data);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to configure device\n");

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data, &tsc1641_chip_info, tsc1641_groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "power monitor %s (Rshunt = %li uOhm)\n",
		 client->name, data->rshunt_uohm);

	return 0;
}

static const struct i2c_device_id tsc1641_id[] = {
	{ "tsc1641", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tsc1641_id);

static const struct of_device_id __maybe_unused tsc1641_of_match[] = {
	{ .compatible = "st,tsc1641" },
	{ },
};
MODULE_DEVICE_TABLE(of, tsc1641_of_match);

static struct i2c_driver tsc1641_driver = {
	.driver = {
		.name = "tsc1641",
		.of_match_table = of_match_ptr(tsc1641_of_match),
	},
	.probe = tsc1641_probe,
	.id_table = tsc1641_id,
};

module_i2c_driver(tsc1641_driver);

MODULE_AUTHOR("Igor Reznichenko <igor@reznichenko.net>");
MODULE_DESCRIPTION("tsc1641 driver");
MODULE_LICENSE("GPL");
