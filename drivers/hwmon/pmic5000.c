// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Jedec PMIC5000 compliant sensors
 *
 * Copyright (c) 2026 Stephen Horvath
 *
 * Inspired by spd5118.c.
 *
 * PMIC5000 compliant sensors are typically used on DDR5 memory modules.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/units.h>

/* PMIC5000 registers. */
// clang-format off
#define PMIC5000_REG_OVER_VOLT_IN	0x08
#define PMIC5000_REG_OVER_CURRENT	0x09
	#define PMIC5000_HIGH_TEMP		BIT(7)
#define PMIC5000_REG_OVER_VOLTAGE	0x0A
#define PMIC5000_REG_UNDER_VOLTAGE	0x0B
#define PMIC5000_REG_SWA_POWER		0x0C
#define PMIC5000_REG_SWB_POWER		0x0D
#define PMIC5000_REG_SWC_POWER		0x0E
#define PMIC5000_REG_SWD_POWER		0x0F
#define PMIC5000_REG_OUTPUT_SELECT	0x1A
	#define PMIC5000_OUTPUT_SELECT		BIT(1)
#define PMIC5000_REG_THRES_AND_SEL	0x1B
	#define PMIC5000_VIN_MGMT_THRESH	BIT(5)
	#define PMIC5000_CURR_OR_PWR		BIT(6)
	#define PMIC5000_VIN_BULK_THRESH	BIT(7)
#define PMIC5000_REG_SWA_CURR_WARN	0x1C
#define PMIC5000_REG_SWB_CURR_WARN	0x1D
#define PMIC5000_REG_SWC_CURR_WARN	0x1E
#define PMIC5000_REG_SWD_CURR_WARN	0x1F
#define PMIC5000_REG_SWA_VOLT_SET	0x21
#define PMIC5000_REG_SWA_THRESH		0x22
#define PMIC5000_REG_SWB_VOLT_SET	0x23
#define PMIC5000_REG_SWB_THRESH		0x24
#define PMIC5000_REG_SWC_VOLT_SET	0x25
#define PMIC5000_REG_SWC_THRESH		0x26
#define PMIC5000_REG_SWD_VOLT_SET	0x27
#define PMIC5000_REG_SWD_THRESH		0x28
#define PMIC5000_REG_SW_VOLT_RANGE	0x2B
	#define PMIC5000_SWD_RANGE		BIT(0)
	#define PMIC5000_SWC_RANGE		BIT(3)
	#define PMIC5000_SWB_RANGE		BIT(4)
	#define PMIC5000_SWA_RANGE		BIT(5)
#define PMIC5000_REG_REGULATOR_CONTROL	0x2F
	#define PMIC5000_SWD_CONTROL		BIT(3)
	#define PMIC5000_SWC_CONTROL		BIT(4)
	#define PMIC5000_SWB_CONTROL		BIT(5)
	#define PMIC5000_SWA_CONTROL		BIT(6)
#define PMIC5000_REG_ADC_CONFIG		0x30
	#define PMIC5000_ADC_SELECT_MASK	GENMASK(6, 3)
	#define PMIC5000_ADC_ENABLE		BIT(7)
#define PMIC5000_REG_ADC_VOLTAGE	0x31
#define PMIC5000_REG_TEMPERATURE	0x33
#define PMIC5000_REG_REVISION		0x3B
#define PMIC5000_REG_VENDOR		0x3C


/* 125 mA multiplier */
#define PMIC5000_CURR_UNIT		125
/* 125 mW multiplier */
#define PMIC5000_POWER_UNIT		(125 * MILLIWATT_PER_WATT)
/* mV multipliers */
#define PMIC5000_VOLT_UNIT		15
#define PMIC5000_VINBULK_UNIT		70
#define PMIC5000_VBIAS_UNIT		25
// clang-format on

struct pmic5000_data {
	struct regmap *regmap;
	struct mutex mode_lock;
	struct mutex adc_lock;
};

static const char *const pmic5000_power_labels[] = { "SWA", "SWB", "SWC",
						     "SWD" };

static const char *const pmic5000_voltage_labels[] = {
	"SWA",	    "SWB",	"SWC",	 "SWD",	      NULL,
	"VIN_Bulk", "VIN_Mgmt", "VBias", "VOUT_1.8V", "VOUT_1.0V"
};

/* hwmon */

static int pmic5000_check_regulator_enabled(struct regmap *regmap, int channel)
{
	u32 regval;
	int err;

	err = regmap_read(regmap, PMIC5000_REG_REGULATOR_CONTROL, &regval);
	if (err)
		return err;
	switch (channel) {
	case 0:
		return !!(regval & PMIC5000_SWA_CONTROL);
	case 1:
		return !!(regval & PMIC5000_SWB_CONTROL);
	case 2:
		return !!(regval & PMIC5000_SWC_CONTROL);
	case 3:
		return !!(regval & PMIC5000_SWD_CONTROL);
	default:
		return -EOPNOTSUPP;
	}
}

static int pmic5000_read_temp(struct regmap *regmap, u32 attr, int channel,
			      long *val)
{
	int err;
	u32 regval;

	if (channel != 0)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_temp_input: {
		err = regmap_read(regmap, PMIC5000_REG_TEMPERATURE, &regval);
		if (err)
			return err;
		regval >>= 5;
		/* Below 85°C */
		if (regval == 0)
			return -EOPNOTSUPP;
		/* 0b001 = 85°C, 0b010 = 95°C, etc. */
		*val = (75 + regval * 10) * MILLIDEGREE_PER_DEGREE;
		return 0;
	}
	case hwmon_temp_max: {
		err = regmap_read(regmap, PMIC5000_REG_THRES_AND_SEL, &regval);
		if (err)
			return err;
		regval &= 0x07;
		/* Reserved */
		if (regval == 0 || regval == 0x7)
			return -EOPNOTSUPP;
		*val = (75 + regval * 10) * MILLIDEGREE_PER_DEGREE;
		return 0;
	}
	case hwmon_temp_max_alarm: {
		err = regmap_read(regmap, PMIC5000_REG_OVER_CURRENT, &regval);
		if (err)
			return err;
		*val = !!(regval & PMIC5000_HIGH_TEMP);
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int pmic5000_read_curr(struct pmic5000_data *data, u32 attr, int channel,
			      long *val)
{
	struct regmap *regmap = data->regmap;
	int reg, err;
	int shift = 0;
	u32 regval;

	if (attr == hwmon_curr_input) {
		mutex_lock(&data->mode_lock);
		/* Select power measurements */
		err = regmap_update_bits(regmap, PMIC5000_REG_THRES_AND_SEL,
					 PMIC5000_CURR_OR_PWR, 0);
		if (err)
			goto error;

		switch (channel) {
		case 0:
			reg = PMIC5000_REG_SWA_POWER;
			break;
		case 1:
			reg = PMIC5000_REG_SWB_POWER;
			break;
		case 2:
			reg = PMIC5000_REG_SWC_POWER;
			break;
		case 3:
			reg = PMIC5000_REG_SWD_POWER;
			break;
		default:
			err = -EOPNOTSUPP;
			goto error;
		}
	} else if (attr == hwmon_curr_max) {
		shift = 2;
		switch (channel) {
		case 0:
			reg = PMIC5000_REG_SWA_CURR_WARN;
			break;
		case 1:
			reg = PMIC5000_REG_SWB_CURR_WARN;
			break;
		case 2:
			reg = PMIC5000_REG_SWC_CURR_WARN;
			break;
		case 3:
			reg = PMIC5000_REG_SWD_CURR_WARN;
			break;
		default:
			return -EOPNOTSUPP;
		}
	} else if (attr == hwmon_curr_max_alarm && channel >= 0 &&
		   channel <= 3) {
		err = regmap_read(regmap, PMIC5000_REG_OVER_CURRENT, &regval);
		if (err)
			return err;
		*val = regval >> (3 - channel) & 0x01;
		return 0;
	} else {
		return -EOPNOTSUPP;
	}

	err = regmap_read(regmap, reg, &regval);
	if (err)
		goto error;

	if (attr == hwmon_curr_input)
		mutex_unlock(&data->mode_lock);

	*val = regval * PMIC5000_CURR_UNIT >> shift;
	return 0;

error:
	if (attr == hwmon_curr_input)
		mutex_unlock(&data->mode_lock);
	return err;
}

static int pmic5000_read_power(struct pmic5000_data *data, u32 attr,
			       int channel, long *val)
{
	struct regmap *regmap = data->regmap;
	int reg, err;
	u32 regval;

	if (attr != hwmon_power_input)
		return -EOPNOTSUPP;

	mutex_lock(&data->mode_lock);
	/* Select power measurements */
	err = regmap_update_bits(regmap, PMIC5000_REG_THRES_AND_SEL,
				 PMIC5000_CURR_OR_PWR, PMIC5000_CURR_OR_PWR);
	if (err)
		goto error;

	switch (channel) {
	case 0:
		reg = PMIC5000_REG_SWA_POWER;
		break;
	case 1:
		reg = PMIC5000_REG_SWB_POWER;
		break;
	case 2:
		reg = PMIC5000_REG_SWC_POWER;
		break;
	case 3:
		reg = PMIC5000_REG_SWD_POWER;
		break;
	default:
		err = -EOPNOTSUPP;
		goto error;
	}

	err = regmap_read(regmap, reg, &regval);
	if (err)
		goto error;

	mutex_unlock(&data->mode_lock);

	*val = regval * PMIC5000_POWER_UNIT;
	return 0;

error:
	mutex_unlock(&data->mode_lock);
	return err;
}

static int pmic5000_read_volt_thresholds(struct regmap *regmap, u32 attr,
					 int channel, long *val)
{
	int err;
	u32 set_reg, thresh_reg, range_bit;
	u32 set_regval, thresh_regval, range_regval;
	u32 volt_set;
	int base_volts[2];

	switch (channel) {
	case 0:
		set_reg = PMIC5000_REG_SWA_VOLT_SET;
		thresh_reg = PMIC5000_REG_SWA_THRESH;
		range_bit = PMIC5000_SWA_RANGE;
		base_volts[0] = 800;
		base_volts[1] = 600;
		break;
	case 1:
		set_reg = PMIC5000_REG_SWB_VOLT_SET;
		thresh_reg = PMIC5000_REG_SWB_THRESH;
		range_bit = PMIC5000_SWB_RANGE;
		base_volts[0] = 800;
		base_volts[1] = 600;
		break;
	case 2:
		set_reg = PMIC5000_REG_SWC_VOLT_SET;
		thresh_reg = PMIC5000_REG_SWC_THRESH;
		range_bit = PMIC5000_SWC_RANGE;
		base_volts[0] = 800;
		base_volts[1] = 600;
		break;
	case 3:
		set_reg = PMIC5000_REG_SWD_VOLT_SET;
		thresh_reg = PMIC5000_REG_SWD_THRESH;
		range_bit = PMIC5000_SWD_RANGE;
		base_volts[0] = 1500;
		base_volts[1] = 2200;
		break;
	case 5:
	case 6: {
		err = regmap_read(regmap, PMIC5000_REG_THRES_AND_SEL,
				  &thresh_regval);
		if (err)
			return err;
		if (channel == 5) {
			if (thresh_regval & PMIC5000_VIN_BULK_THRESH)
				*val = 14500;
			else
				*val = 16000;
			return 0;
		} else {
			if (thresh_regval & PMIC5000_VIN_MGMT_THRESH)
				*val = 3800;
			else
				*val = 3700;
			return 0;
		}
	}

	default:
		return -EOPNOTSUPP;
	}

	err = regmap_read(regmap, set_reg, &set_regval);
	if (err)
		return err;
	err = regmap_read(regmap, thresh_reg, &thresh_regval);
	if (err)
		return err;
	err = regmap_read(regmap, PMIC5000_REG_SW_VOLT_RANGE, &range_regval);
	if (err)
		return err;

	volt_set = range_regval & range_bit ? base_volts[1] : base_volts[0];
	volt_set += (set_regval >> 1) * 5;

	switch (attr) {
	case hwmon_in_min:
		/* 10%, 12.5%, Reserved, Reserved */
		const int min_permilles[4] = { 100, 125, PERMILLE, PERMILLE };
		*val = volt_set - (min_permilles[(thresh_regval >> 2) & 0x03] *
				   volt_set / PERMILLE);
		return 0;
	case hwmon_in_max:
		/* 7.5%, 10%, 12.5%, Reserved */
		const int max_permilles[4] = { 75, 100, 125, PERMILLE };
		*val = volt_set + (max_permilles[(thresh_regval >> 4) & 0x03] *
				   volt_set / PERMILLE);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int pmic5000_read_adc_alarms(struct regmap *regmap, u32 attr,
				    int channel, long *val)
{
	int err;
	u32 regval;

	if (channel >= 0 && channel <= 3) {
		switch (attr) {
		case hwmon_in_min_alarm: {
			err = regmap_read(regmap, PMIC5000_REG_UNDER_VOLTAGE,
					  &regval);
			if (err)
				return err;
			*val = regval >> (3 - channel) & 0x01;
			return 0;
		}
		case hwmon_in_max_alarm: {
			err = regmap_read(regmap, PMIC5000_REG_OVER_VOLTAGE,
					  &regval);
			if (err)
				return err;
			*val = regval >> (7 - channel) & 0x01;
			return 0;
		}
		}
	} else if (channel == 5 || channel == 6) {
		switch (attr) {
		case hwmon_in_max_alarm: {
			err = regmap_read(regmap, PMIC5000_REG_OVER_VOLT_IN,
					  &regval);
			if (err)
				return err;
			*val = regval >> (channel - 4) & 0x01;
			return 0;
		}
		}
	}

	return -EOPNOTSUPP;
}

static int pmic5000_read_adc(struct pmic5000_data *data, u32 attr, int channel,
			     long *val)
{
	struct regmap *regmap = data->regmap;
	int err, mult;
	u32 regval;

	switch (attr) {
	case hwmon_in_enable:
		err = pmic5000_check_regulator_enabled(regmap, channel);
		if (err < 0)
			return err;
		*val = err;
		return 0;
	case hwmon_in_input:
		break;
	case hwmon_in_min:
	case hwmon_in_max:
		return pmic5000_read_volt_thresholds(regmap, attr, channel,
						     val);
	case hwmon_in_min_alarm:
	case hwmon_in_max_alarm:
		return pmic5000_read_adc_alarms(regmap, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}

	/* Channel 4 is reserved */
	if (channel < 0 || channel > 9 || channel == 4)
		return -EOPNOTSUPP;

	switch (channel) {
	case 5:
		mult = PMIC5000_VINBULK_UNIT;
		break;
	case 7:
		mult = PMIC5000_VBIAS_UNIT;
		break;
	default:
		mult = PMIC5000_VOLT_UNIT;
		break;
	}

	mutex_lock(&data->adc_lock);

	err = regmap_update_bits(regmap, PMIC5000_REG_ADC_CONFIG,
				 PMIC5000_ADC_SELECT_MASK, channel << 3);
	if (err)
		goto error;

	/*
	 * The host shall wait minimum of 9 ms delay after the input selection
	 * for ADC readout and the actual readout
	 *
	 * msleep may sleep for up to 20ms, which is fine.
	 */
	msleep(9);

	err = regmap_read(regmap, PMIC5000_REG_ADC_VOLTAGE, &regval);
	if (err)
		goto error;

	mutex_unlock(&data->adc_lock);

	*val = regval * mult;
	return 0;

error:
	mutex_unlock(&data->adc_lock);
	return err;
}

static int pmic5000_read_interval(struct regmap *regmap, u32 attr, long *val)
{
	unsigned int regval;
	int err;

	if (attr != hwmon_chip_update_interval)
		return -EOPNOTSUPP;

	err = regmap_read(regmap, PMIC5000_REG_ADC_CONFIG, &regval);
	if (err < 0)
		return err;
	*val = 1 << (regval & 0x03);
	return 0;
}

static int pmic5000_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct pmic5000_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;

	switch (type) {
	case hwmon_chip:
		return pmic5000_read_interval(regmap, attr, val);
	case hwmon_temp:
		return pmic5000_read_temp(regmap, attr, channel, val);
	case hwmon_in:
		return pmic5000_read_adc(data, attr, channel, val);
	case hwmon_curr:
		return pmic5000_read_curr(data, attr, channel, val);
	case hwmon_power:
		return pmic5000_read_power(data, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int pmic5000_read_string(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, const char **str)
{
	if (type == hwmon_curr && attr == hwmon_curr_label) {
		if (channel < 0 || channel > 3)
			return -EOPNOTSUPP;
		*str = pmic5000_power_labels[channel];
	} else if (type == hwmon_power && attr == hwmon_power_label) {
		if (channel < 0 || channel > 3)
			return -EOPNOTSUPP;
		*str = pmic5000_power_labels[channel];
	} else if (type == hwmon_in && attr == hwmon_in_label) {
		if (channel < 0 || channel > 9 || channel == 4)
			return -EOPNOTSUPP;
		*str = pmic5000_voltage_labels[channel];
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int pmic5000_write_interval(struct pmic5000_data *data, long val)
{
	struct regmap *regmap = data->regmap;
	u32 regval;
	int err;

	switch (val) {
	case 1:
		regval = 0;
		break;
	case 2:
		regval = 1;
		break;
	case 4:
		regval = 2;
		break;
	case 8:
		regval = 3;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->adc_lock);
	err = regmap_update_bits(regmap, PMIC5000_REG_ADC_CONFIG, 0x03, regval);
	mutex_unlock(&data->adc_lock);
	return err;
}

static int pmic5000_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct pmic5000_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_chip:
		return pmic5000_write_interval(data, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t pmic5000_is_visible(const void *data,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel)
{
	int ret;
	struct pmic5000_data *pmic_data = (struct pmic5000_data *)data;
	struct regmap *regmap = pmic_data->regmap;

	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_update_interval)
			return 0644;
		break;
	case hwmon_temp:
		return 0444;
	case hwmon_in:
		if (channel == 4)
			return 0;
		if (channel >= 0 && channel <= 3 && (attr != hwmon_in_enable)) {
			ret = pmic5000_check_regulator_enabled(regmap, channel);
			if (!ret || ret < 0)
				return 0;
		}
		return 0444;
	case hwmon_power:
		if (channel >= 0 && channel <= 3) {
			ret = pmic5000_check_regulator_enabled(regmap, channel);
			if (!ret || ret < 0)
				return 0;
		}
		return 0444;
	case hwmon_curr:
		if (channel >= 0 && channel <= 3) {
			ret = pmic5000_check_regulator_enabled(regmap, channel);
			if (!ret || ret < 0)
				return 0;
		}
		return 0444;
	default:
		break;
	}
	return 0444;
}

/*
 * Bank and vendor id are 8-bit fields with seven data bits and odd parity.
 * Vendor IDs 0 and 0x7f are invalid.
 * See Jedec standard JEP106BJ for details and a list of assigned vendor IDs.
 */
static bool pmic5000_vendor_valid(u8 bank, u8 id)
{
	if (parity8(bank) == 0 || parity8(id) == 0)
		return false;

	id &= 0x7f;
	return id && id != 0x7f;
}

static const struct hwmon_channel_info *pmic5000_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_ALARM),
	HWMON_CHANNEL_INFO(
		in,
		HWMON_I_ENABLE | HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX |
			HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_ENABLE | HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX |
			HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_ENABLE | HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX |
			HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_ENABLE | HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX |
			HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_INPUT,
		HWMON_I_INPUT | HWMON_I_MAX | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_MAX | HWMON_I_MAX_ALARM | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(
		curr,
		HWMON_C_INPUT | HWMON_C_MAX | HWMON_C_MAX_ALARM | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_MAX | HWMON_C_MAX_ALARM | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_MAX | HWMON_C_MAX_ALARM | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_MAX | HWMON_C_MAX_ALARM |
			HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops pmic5000_hwmon_ops = {
	.is_visible = pmic5000_is_visible,
	.read = pmic5000_read,
	.read_string = pmic5000_read_string,
	.write = pmic5000_write,
};

static const struct hwmon_chip_info pmic5000_chip_info = {
	.ops = &pmic5000_hwmon_ops,
	.info = pmic5000_info,
};

/* regmap */

static bool pmic5000_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PMIC5000_REG_OUTPUT_SELECT:
	case PMIC5000_REG_THRES_AND_SEL:
	case PMIC5000_REG_ADC_CONFIG:
		return true;
	default:
		return false;
	}
}

static bool pmic5000_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PMIC5000_REG_OVER_VOLT_IN:
	case PMIC5000_REG_OVER_CURRENT:
	case PMIC5000_REG_OVER_VOLTAGE:
	case PMIC5000_REG_UNDER_VOLTAGE:
	case PMIC5000_REG_SWA_POWER:
	case PMIC5000_REG_SWB_POWER:
	case PMIC5000_REG_SWC_POWER:
	case PMIC5000_REG_SWD_POWER:
	case PMIC5000_REG_ADC_VOLTAGE:
	case PMIC5000_REG_TEMPERATURE:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config pmic5000_regmap8_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x3f,
	.writeable_reg = pmic5000_writeable_reg,
	.volatile_reg = pmic5000_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static int pmic5000_suspend(struct device *dev)
{
	struct pmic5000_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	u32 regval;
	int err;

	/*
	 * Make sure the configuration register in the regmap cache is current
	 * before bypassing it.
	 */
	err = regmap_read(regmap, PMIC5000_REG_ADC_CONFIG, &regval);
	if (err < 0)
		return err;

	regcache_cache_bypass(regmap, true);
	regmap_update_bits(regmap, PMIC5000_REG_ADC_CONFIG, PMIC5000_ADC_ENABLE,
			   0);
	regcache_cache_bypass(regmap, false);

	regcache_cache_only(regmap, true);
	regcache_mark_dirty(regmap);

	return 0;
}

static int pmic5000_resume(struct device *dev)
{
	struct pmic5000_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;

	regcache_cache_only(regmap, false);
	return regcache_sync(regmap);
}

static DEFINE_SIMPLE_DEV_PM_OPS(pmic5000_pm_ops, pmic5000_suspend,
				pmic5000_resume);

static int pmic5000_common_probe(struct device *dev, struct regmap *regmap)
{
	unsigned int revision, vendor, bank;
	struct pmic5000_data *data;
	struct device *hwmon_dev;
	int err;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = regmap_read(regmap, PMIC5000_REG_REVISION, &revision);
	if (err)
		return err;

	err = regmap_read(regmap, PMIC5000_REG_VENDOR, &bank);
	if (err)
		return err;
	err = regmap_read(regmap, PMIC5000_REG_VENDOR + 1, &vendor);
	if (err)
		return err;
	if (!pmic5000_vendor_valid(bank, vendor))
		return -ENODEV;

	data->regmap = regmap;
	mutex_init(&data->mode_lock);
	mutex_init(&data->adc_lock);
	dev_set_drvdata(dev, data);

	hwmon_dev = devm_hwmon_device_register_with_info(
		dev, "pmic5000", data, &pmic5000_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "DDR5 PMIC sensor: vendor 0x%02x:0x%02x revision %d.%d\n",
		 bank & 0x7f, vendor, ((revision >> 4) & 0x03) + 1,
		 ((revision >> 1) & 0x07) + 1);

	/* Enable individual measurements and enable ADC */
	err = regmap_update_bits(regmap, PMIC5000_REG_OUTPUT_SELECT,
				 PMIC5000_OUTPUT_SELECT,
				 PMIC5000_OUTPUT_SELECT);
	if (err)
		return err;
	err = regmap_update_bits(regmap, PMIC5000_REG_ADC_CONFIG,
				 PMIC5000_ADC_ENABLE, PMIC5000_ADC_ENABLE);
	if (err)
		return err;

	return 0;
}

/* I2C */

static int pmic5000_i2c_init(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;

	/*
	 * Register accesses are 8-bit, so require byte-data transactions only.
	 * Requiring WORD_DATA here rejects otherwise valid adapters.
	 */
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	return 0;
}

static int pmic5000_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	int err;

	err = pmic5000_i2c_init(client);
	if (err)
		return dev_err_probe(dev, err, "I2C capability check failed\n");

	regmap = devm_regmap_init_i2c(client, &pmic5000_regmap8_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "regmap init failed\n");

	return pmic5000_common_probe(dev, regmap);
}

static const struct i2c_device_id pmic5000_i2c_id[] = { { .name = "pmic5000" },
							{} };
MODULE_DEVICE_TABLE(i2c, pmic5000_i2c_id);

static const struct of_device_id pmic5000_of_ids[] = {
	{
		.compatible = "jedec,pmic5000",
	},
	{}
};
MODULE_DEVICE_TABLE(of, pmic5000_of_ids);

static struct i2c_driver pmic5000_i2c_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "pmic5000",
		.of_match_table = pmic5000_of_ids,
		.pm = pm_sleep_ptr(&pmic5000_pm_ops),
	},
	.probe		= pmic5000_i2c_probe,
	.id_table	= pmic5000_i2c_id,
};

module_i2c_driver(pmic5000_i2c_driver);

MODULE_AUTHOR("Stephen Horvath <linux@stevetech.au>");
MODULE_DESCRIPTION("PMIC5000 driver");
MODULE_LICENSE("GPL");
