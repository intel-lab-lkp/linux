// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * emc2103.c - Support for SMSC EMC2103
 * Copyright (c) 2010 SMSC
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>

/* Addresses scanned */
static const unsigned short normal_i2c[] = { 0x2E, I2C_CLIENT_END };

static const u8 REG_TEMP_MIN[4] = { 0x3c, 0x38, 0x39, 0x3a };
static const u8 REG_TEMP_MAX[4] = { 0x34, 0x30, 0x31, 0x32 };
static const u8 REG_TEMP_CRIT[4] = { 0x1d, 0x19, 0x1a, 0x1b };

#define REG_TEMP(n)		((n) * 2)
#define REG_TEMP_EMERGENCY	0x0a
#define REG_TEMP_CRIT_ALARM	0x1f
#define REG_CONF1		0x20
#define REG_TEMP_MAX_ALARM	0x24
#define REG_TEMP_MIN_ALARM	0x25
#define REG_FAN_STATUS		0x27
#define REG_PWM_BASE_FREQUENCY	0x2b
#define REG_PWM_DUTY		0x40
#define REG_PWM_FREQ_DIVIDE	0x41
#define REG_FAN_CONF1		0x42
#define REG_FAN_VALID_TACH	0x49
#define REG_FAN_TARGET_LO	0x4c
#define REG_FAN_TARGET_HI	0x4d
#define REG_FAN_TACH_HI		0x4e
#define REG_FAN_TACH_LO		0x4f
#define REG_PRODUCT_ID		0xfd
#define REG_MFG_ID		0xfe

#define FAN_AUTO_MASK		BIT(7)
#define FAN_DIV_MASK		GENMASK(6, 5)
#define FAN_PULSES_MASK		GENMASK(4, 3)

#define FAN_BASE_FREQ_MASK	GENMASK(1, 0)

/* equation 4 from datasheet: rpm = (3932160 * multiplier) / count */
#define FAN_RPM_FACTOR		3932160

/*
 * 2103-2 and 2103-4's 3rd temperature sensor can be connected to two diodes
 * in anti-parallel mode, and in this configuration both can be read
 * independently (so we have 4 temperature inputs).  The device can't
 * detect if it's connected in this mode, so we have to manually enable
 * it.  Default is to leave the device in the state it's already in (-1).
 * This parameter allows APD mode to be optionally forced on or off
 */
static int apd = -1;
module_param(apd, bint, 0);
MODULE_PARM_DESC(apd, "Set to zero to disable anti-parallel diode mode");

struct emc2103_data {
	struct regmap	*regmap;
	struct mutex	update_lock;
	int		temp_count;	/* num of temp sensors */
};

static long rpm_from_reg(u16 regval, u8 conf)
{
	int multiplier;

	if (!regval)
		return 0;

	multiplier = BIT(FIELD_GET(FAN_DIV_MASK, conf));
	return FAN_RPM_FACTOR * multiplier / regval;
}

static u16 rpm_to_reg(long rpm, u8 conf)
{
	int multiplier;

	/* Datasheet states 16384 as maximum RPM target (table 3.2) */
	rpm = clamp_val(rpm, 0, 16384);
	if (!rpm)
		return 0;

	multiplier = BIT(FIELD_GET(FAN_DIV_MASK, conf));
	return clamp_val(FAN_RPM_FACTOR * multiplier / rpm, 0, 0x1fff);
}

static int emc2103_temp_read(struct emc2103_data *data, u32 attr, int channel, long *val)
{
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	u8 regvals[2];
	int ret, temp;

	switch (attr) {
	case hwmon_temp_input:
		ret = regmap_bulk_read(regmap, REG_TEMP(channel), regvals, 2);
		if (ret)
			return ret;
		temp = sign_extend32(regvals[0] << 8 | regvals[1], 15);
		*val = DIV_ROUND_CLOSEST(temp * 125, 32);
		break;
	case hwmon_temp_min:
		ret = regmap_read(regmap, REG_TEMP_MIN[channel], &regval);
		if (ret)
			return ret;
		*val = sign_extend32(regval, 7) * 1000;
		break;
	case hwmon_temp_max:
		ret = regmap_read(regmap, REG_TEMP_MAX[channel], &regval);
		if (ret)
			return ret;
		*val = sign_extend32(regval, 7) * 1000;
		break;
	case hwmon_temp_crit:
		ret = regmap_read(regmap, REG_TEMP_CRIT[channel], &regval);
		if (ret)
			return ret;
		*val = sign_extend32(regval, 7) * 1000;
		break;
	case hwmon_temp_emergency:
		ret = regmap_read(regmap, REG_TEMP_EMERGENCY, &regval);
		if (ret)
			return ret;
		*val = regval * 1000;
		break;
	case hwmon_temp_min_alarm:
		ret = regmap_read(regmap, REG_TEMP_MIN_ALARM, &regval);
		if (ret)
			return ret;
		*val = !!(regval & BIT(channel));
		break;
	case hwmon_temp_max_alarm:
		ret = regmap_read(regmap, REG_TEMP_MAX_ALARM, &regval);
		if (ret)
			return ret;
		*val = !!(regval & BIT(channel));
		break;
	case hwmon_temp_crit_alarm:
		ret = regmap_read(regmap, REG_TEMP_CRIT_ALARM, &regval);
		if (ret)
			return ret;
		*val = !!(regval & BIT(channel));
		break;
	case hwmon_temp_fault:
		ret = regmap_read(regmap, REG_TEMP(channel), &regval);
		if (ret)
			return ret;
		*val = (regval == BIT(7));
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int emc2103_fan_read(struct emc2103_data *data, u32 attr, long *val)
{
	struct regmap *regmap = data->regmap;
	static unsigned int regs_input[3] = {
			REG_FAN_TACH_LO, REG_FAN_TACH_HI, REG_FAN_CONF1 };
	static unsigned int regs_target[3] = {
			REG_FAN_TARGET_LO, REG_FAN_TARGET_HI, REG_FAN_CONF1 };
	static unsigned int regs_min[2] = {
			REG_FAN_TARGET_HI, REG_FAN_CONF1 };
	u8 regvals[3];
	u32 regval;
	int ret;

	switch (attr) {
	case hwmon_fan_input:
		ret = regmap_multi_reg_read(regmap, regs_input, regvals, 3);
		if (ret)
			return ret;
		*val = rpm_from_reg((regvals[1] << 5) | (regvals[0] >> 3), regvals[2]);
		break;
	case hwmon_fan_target:
		ret = regmap_multi_reg_read(regmap, regs_target, regvals, 3);
		if (ret)
			return ret;
		if (regvals[1] == 0xff)	/* disabled */
			*val = 0;
		else
			*val = rpm_from_reg((regvals[1] << 5) | (regvals[0] >> 3), regvals[2]);
		break;
	case hwmon_fan_fault:
		ret = regmap_read(regmap, REG_FAN_STATUS, &regval);
		if (ret)
			return ret;
		*val = !!(regval & BIT(1));
		break;
	case hwmon_fan_div:
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			return ret;

		*val = BIT(3 - FIELD_GET(FAN_DIV_MASK, regval));
		break;
	case hwmon_fan_pulses:
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			return ret;
		*val = FIELD_GET(FAN_PULSES_MASK, regval) + 1;
		break;
	case hwmon_fan_min:
		ret = regmap_multi_reg_read(regmap, regs_min, regvals, 2);
		if (ret)
			return ret;
		*val = rpm_from_reg((regvals[0] << 5), regvals[1]);
		break;
	case hwmon_fan_min_alarm:
		ret = regmap_read(regmap, REG_FAN_STATUS, &regval);
		if (ret)
			return ret;
		*val = !!(regval & BIT(0));
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static const u16 pwm_frequencies[] = {26000, 19531, 4882, 2441};

static int emc2103_pwm_read(struct emc2103_data *data, u32 attr, long *val)
{
	unsigned int regs[2] = {REG_PWM_BASE_FREQUENCY, REG_PWM_FREQ_DIVIDE};
	struct regmap *regmap = data->regmap;
	u8 regvals[2];
	u32 regval;
	int ret;

	switch (attr) {
	case hwmon_pwm_input:
		ret = regmap_read(regmap, REG_PWM_DUTY, &regval);
		if (ret)
			return ret;
		*val = regval;
		break;
	case hwmon_pwm_enable:
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			return ret;
		*val = (regval & FAN_AUTO_MASK) ? 3 : 0;
		break;
	case hwmon_pwm_freq:
		ret = regmap_multi_reg_read(regmap, regs, regvals, 2);
		if (ret)
			return ret;
		*val = pwm_frequencies[regvals[0] & FAN_BASE_FREQ_MASK] / (regvals[1] ? : 1);
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int emc2103_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct emc2103_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return emc2103_temp_read(data, attr, channel, val);
	case hwmon_fan:
		return emc2103_fan_read(data, attr, val);
	case hwmon_pwm:
		return emc2103_pwm_read(data, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int emc2103_temp_write(struct emc2103_data *data, u32 attr, int channel, long val)
{
	struct regmap *regmap = data->regmap;

	val = DIV_ROUND_CLOSEST(clamp_val(val, -128000, 127000), 1000);

	switch (attr) {
	case hwmon_temp_min:
		return regmap_write(regmap, REG_TEMP_MIN[channel], val);
	case hwmon_temp_max:
		return regmap_write(regmap, REG_TEMP_MAX[channel], val);
	case hwmon_temp_crit:
		return regmap_write(regmap, REG_TEMP_CRIT[channel], val);
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int emc2103_fan_write(struct emc2103_data *data, u32 attr, long val)
{
	struct regmap *regmap = data->regmap;
	unsigned int regval;
	u8 regvals[2];
	int ret = 0;
	int old_mul;

	mutex_lock(&data->update_lock);

	switch (attr) {
	case hwmon_fan_target:
		if (val < 0) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			break;
		if (val == 0)
			val = 0x01fff;
		else
			val = rpm_to_reg(val, regval);
		regvals[0] = (val << 3) & 0xff;
		regvals[1] = val >> 5;
		ret = regmap_bulk_write(regmap, REG_FAN_TARGET_LO, regvals, 2);
		break;
	case hwmon_fan_min:
		if (val < 0) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			break;
		val = rpm_to_reg(val, regval);
		ret = regmap_write(regmap, REG_FAN_VALID_TACH, val >> 5);
		break;
	case hwmon_fan_div:
		if (val <= 0 || val > 8 || hweight32(val) != 1) {
			ret = -EINVAL;
			break;
		}
		val = 8 / val;	/* convert divider to multiplier */
		/*
		 * Note: we also update the fan target here, because its value is
		 * determined in part by the fan clock divider.  This follows the principle
		 * of least surprise; the user doesn't expect the fan target to change just
		 * because the divider changed.
		 */
		ret = regmap_read(regmap, REG_FAN_CONF1, &regval);
		if (ret)
			break;
		old_mul = BIT(FIELD_GET(FAN_DIV_MASK, regval));
		ret = regmap_update_bits(regmap, REG_FAN_CONF1, FAN_DIV_MASK,
					 FIELD_PREP(FAN_DIV_MASK, __ffs(val)));
		if (ret)
			break;
		ret = regmap_bulk_read(regmap, REG_FAN_TARGET_LO, regvals, 2);
		if (ret)
			break;
		regval = (regvals[1] << 5) | (regvals[0] >> 3);
		val = regval * val / old_mul;
		regvals[0] = (val << 3) & 0xff;
		regvals[1] = val >> 5;
		ret = regmap_bulk_write(regmap, REG_FAN_TARGET_LO, regvals, 2);
		break;
	case hwmon_fan_pulses:
		if (val < 1 || val > 4) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_update_bits(regmap, REG_FAN_CONF1, FAN_PULSES_MASK,
					 FIELD_PREP(FAN_PULSES_MASK, val + 1));
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&data->update_lock);
	return ret;
}

static int emc2103_pwm_write(struct emc2103_data *data, u32 attr, long val)
{
	struct regmap *regmap = data->regmap;

	switch (attr) {
	case hwmon_pwm_enable:
		if (val && val != 3)
			return -EINVAL;
		return regmap_update_bits(regmap, REG_FAN_CONF1, FAN_AUTO_MASK,
					  val ? FAN_AUTO_MASK : 0);
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		return regmap_write(regmap, REG_PWM_DUTY, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int emc2103_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct emc2103_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return emc2103_temp_write(data, attr, channel, val);
	case hwmon_fan:
		return emc2103_fan_write(data, attr, val);
	case hwmon_pwm:
		return emc2103_pwm_write(data, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t emc2103_is_visible(const void *_data, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	const struct emc2103_data *data = _data;

	switch (type) {
	case hwmon_temp:
		if (channel >= data->temp_count)
			return 0;
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_fault:
		case hwmon_temp_min_alarm:
		case hwmon_temp_max_alarm:
		case hwmon_temp_crit_alarm:
		case hwmon_temp_emergency:
			return 0444;
		case hwmon_temp_min:
		case hwmon_temp_max:
		case hwmon_temp_crit:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_fault:
		case hwmon_fan_min_alarm:
			return 0444;
		case hwmon_fan_div:
		case hwmon_fan_pulses:
		case hwmon_fan_target:
		case hwmon_fan_min:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
		case hwmon_pwm_input:
			return 0644;
		case hwmon_pwm_freq:
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

static const struct hwmon_channel_info * const emc2103_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_FAULT | HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_CRIT | HWMON_T_CRIT_ALARM | HWMON_T_EMERGENCY,
			   HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_FAULT | HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_CRIT | HWMON_T_CRIT_ALARM,
			   HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_FAULT | HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_CRIT | HWMON_T_CRIT_ALARM,
			   HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_FAULT | HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_CRIT | HWMON_T_CRIT_ALARM),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_DIV | HWMON_F_TARGET |
			   HWMON_F_PULSES | HWMON_F_FAULT | HWMON_F_MIN |
			   HWMON_F_MIN_ALARM),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_FREQ),
	NULL
};

static const struct hwmon_ops emc2103_hwmon_ops = {
	.is_visible = emc2103_is_visible,
	.read = emc2103_read,
	.write = emc2103_write,
};

static const struct hwmon_chip_info emc2103_chip_info = {
	.ops = &emc2103_hwmon_ops,
	.info = emc2103_info,
};

static bool emc2103_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00 ... 0x07:	/* temperature registers */
	case REG_TEMP_CRIT_ALARM:
	case REG_TEMP_MAX_ALARM:
	case REG_TEMP_MIN_ALARM:
	case REG_FAN_TACH_HI:
	case REG_FAN_TACH_LO:
	case REG_FAN_STATUS:
	case REG_PWM_DUTY:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config emc2103_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_reg = emc2103_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static int emc2103_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct emc2103_data *data;
	struct device *hwmon_dev;
	struct regmap *regmap;
	u32 regval;
	int ret;

	data = devm_kzalloc(&client->dev, sizeof(struct emc2103_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &emc2103_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data->regmap = regmap;
	mutex_init(&data->update_lock);

	/* 2103-2 and 2103-4 have 3 external diodes, 2103-1 has 1 */
	ret = regmap_read(regmap, REG_PRODUCT_ID, &regval);
	if (ret)
		return ret;

	if (regval == 0x24) {
		/* 2103-1 only has 1 external diode */
		data->temp_count = 2;
	} else {
		/* 2103-2 and 2103-4 have 3 or 4 external diodes */

		/* force APD state if module parameter is set */
		switch (apd) {
		case 0:
			/* force APD mode off */
			data->temp_count = 3;
			ret = regmap_clear_bits(regmap, REG_CONF1, BIT(0));
			if (ret)
				return ret;
			break;
		case 1:
			/* force APD mode on */
			data->temp_count = 4;
			ret = regmap_set_bits(regmap, REG_CONF1, BIT(0));
			if (ret)
				return ret;
			break;
		default:
			ret = regmap_read(regmap, REG_CONF1, &regval);
			if (ret < 0)
				return ret;

			/* detect current state of hardware */
			data->temp_count = (regval & BIT(0)) ? 4 : 3;
			break;
		}
	}

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, data,
							 &emc2103_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);
	return 0;
}

static const struct i2c_device_id emc2103_ids[] = {
	{ "emc2103" },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, emc2103_ids);

/* Return 0 if detection is successful, -ENODEV otherwise */
static int
emc2103_detect(struct i2c_client *new_client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = new_client->adapter;
	int manufacturer, product;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	manufacturer = i2c_smbus_read_byte_data(new_client, REG_MFG_ID);
	if (manufacturer != 0x5D)
		return -ENODEV;

	product = i2c_smbus_read_byte_data(new_client, REG_PRODUCT_ID);
	if (product != 0x24 && product != 0x26)
		return -ENODEV;

	strscpy(info->type, "emc2103", I2C_NAME_SIZE);

	return 0;
}

static struct i2c_driver emc2103_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "emc2103",
	},
	.probe		= emc2103_probe,
	.id_table	= emc2103_ids,
	.detect		= emc2103_detect,
	.address_list	= normal_i2c,
};

module_i2c_driver(emc2103_driver);

MODULE_AUTHOR("Steve Glendinning <steve.glendinning@shawell.net>");
MODULE_DESCRIPTION("SMSC EMC2103 hwmon driver");
MODULE_LICENSE("GPL");
