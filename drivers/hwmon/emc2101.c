// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Microchip EMC2101 fan controller.
 *
 * Copyright 2025 Álvaro Fernández Rojas <noltari@gmail.com>
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/util_macros.h>

#define REG_TEMP_INT			0x00
#define REG_TEMP_EXT_HI			0x01
#define REG_STATUS			0x02
#define  ADC_BUSY			BIT(7)
#define  TEMP_INT_HIGH			BIT(6)
#define  EEPROM_ERROR			BIT(5)
#define  TEMP_EXT_HIGH			BIT(4)
#define  TEMP_EXT_LOW			BIT(3)
#define  TEMP_EXT_FAULT			BIT(2)
#define  TEMP_EXT_CRIT			BIT(1)
#define  TACH_LOW			BIT(0)
#define REG_CONFIG			0x03
#define  ALERT_IRQ_ACK			BIT(7)
#define  FAN_STANDBY_ENABLE		BIT(6)
#define  FAN_STANDBY_MODE		BIT(5)
#define  FAN_MODE_DAC			BIT(4)
#define  SMBUS_TOUT_DISABLE		BIT(3)
#define  PIN_FUNC_TACH			BIT(2)
#define  TEMP_EXT_CRIT_UNLOCK		BIT(1)
#define  PIN_ASSERT_3_EXC		BIT(0)
#define REG_CONV_RATE			0x04
#define  CONV_RATE_MASK			0xf
#define REG_TEMP_INT_MAX		0x05
#define REG_TEMP_EXT_MAX_HI		0x07
#define REG_TEMP_EXT_MIN_HI		0x08
#define REG_TEMP_EXT_FORCE		0x0c
#define REG_ONE_SHOT			0x0f
#define REG_TEMP_EXT_LO			0x10
#define REG_SCRATCHPAD_1		0x11
#define REG_SCRATCHPAD_2		0x12
#define REG_TEMP_EXT_MAX_LO		0x13
#define REG_TEMP_EXT_MIN_LO		0x14
#define REG_ALERT_MASK			0x16
#define  IRQ_TEMP_INT_MAX_DISABLE	BIT(6)
#define  IRQ_TEMP_EXT_MAX_DISABLE	BIT(4)
#define  IRQ_TEMP_EXT_MIN_DISABLE	BIT(3)
#define  IRQ_TEMP_EXT_CRIT_DISABLE	BIT(1)
#define  IRQ_TACH_MIN_DISABLE		BIT(0)
#define REG_EXT_IDEALITY		0x17
#define  EXT_IDEALITY_START		9846
#define  EXT_IDEALITY_STEP		13
#define  EXT_IDEALITY_VAL(x)		(EXT_IDEALITY_START + \
					 ((x) * EXT_IDEALITY_STEP))
#define  EXT_IDEALITY_MASK		0x3f
#define REG_BETA_COMP			0x18
#define  BETA_COMP_AUTO			BIT(3)
#define  BETA_COMP_DISABLE		7
#define  BETA_COMP_2_33			6
#define  BETA_COMP_1_00			5
#define  BETA_COMP_0_43			4
#define  BETA_COMP_0_33			3
#define  BETA_COMP_0_25			2
#define  BETA_COMP_0_18			1
#define  BETA_COMP_0_11			0
#define  BETA_COMP_MASK			0x7
#define REG_TEMP_EXT_CRIT		0x19
#define REG_TEMP_EXT_CRIT_HYST		0x21
#define REG_TACH_LO			0x46
#define REG_TACH_HI			0x47
#define REG_TACH_MIN_LO			0x48
#define REG_TACH_MIN_HI			0x49
#define REG_FAN_CONFIG			0x4a
#define  FAN_EXT_TEMP_FORCE		BIT(6)
#define  FAN_LUT_DISABLE		BIT(5)
#define  FAN_POL_INV			BIT(4)
#define  FAN_CLK_SEL			BIT(3)
#define  FAN_CLK_OVR			BIT(2)
#define  TACH_FALSE_READ_DISABLE	BIT(0)
#define  TACH_FALSE_READ_MASK		0x3
#define REG_FAN_SPIN			0x4b
#define  FAN_SPIN_UP_ABORT		BIT(5)
#define  FAN_SPIN_UP_POWER_SHIFT	3
#define  FAN_SPIN_UP_POWER_100		(3 << FAN_SPIN_UP_POWER_SHIFT)
#define  FAN_SPIN_UP_POWER_75		(2 << FAN_SPIN_UP_POWER_SHIFT)
#define  FAN_SPIN_UP_POWER_50		(1 << FAN_SPIN_UP_POWER_SHIFT)
#define  FAN_SPIN_UP_POWER_0		(0 << FAN_SPIN_UP_POWER_SHIFT)
#define  FAN_SPIN_UP_POWER_MASK		(0x3 << FAN_SPIN_UP_POWER_SHIFT)
#define  FAN_SPIN_UP_TIME_3200		7
#define  FAN_SPIN_UP_TIME_1600		6
#define  FAN_SPIN_UP_TIME_800		5
#define  FAN_SPIN_UP_TIME_400		4
#define  FAN_SPIN_UP_TIME_200		3
#define  FAN_SPIN_UP_TIME_100		2
#define  FAN_SPIN_UP_TIME_50		1
#define  FAN_SPIN_UP_TIME_0		0
#define  FAN_SPIN_UP_TIME_MASK		0x7
#define REG_FAN_SET			0x4c
#define  FAN_SET_MASK			0x3f
#define REG_PWM_FREQ			0x4d
#define  PWM_FREQ_MASK			0x1f
#define REG_PWM_FREQ_DIV		0x4e
#define REG_FAN_LUT_HYST		0x4f
#define  FAN_LUT_HYST_MASK		0x1f
#define REG_FAN_LUT_TEMP(x)		(0x50 + (0x2 * (x)))
/* Write only with FAN_LUT_DISABLE */
#define  FAN_LUT_TEMP_MASK		0x7f
#define REG_FAN_LUT_SPEED(x)		(0x51 + (0x2 * (x)))
/* Write only with FAN_LUT_DISABLE */
#define  FAN_LUT_SPEED_MASK		0x3f
#define REG_AVG_FILTER			0xbf
#define  FILTER_SHIFT			1
#define  FILTER_L2			(3 << FILTER_SHIFT)
#define  FILTER_L1			(1 << FILTER_SHIFT)
#define  FILTER_NONE			(0 << FILTER_SHIFT)
#define  FILTER_MASK			(0x3 << FILTER_SHIFT)
#define  ALERT_PIN_TEMP_COMP		BIT(0)
#define REG_PRODUCT_ID			0xfd
#define REG_MANUFACTURER_ID		0xfe
#define REG_REVISION			0xff

#define CLK_FREQ_ALT			1400
#define CLK_FREQ_BASE			360000

#define FAN_LUT_COUNT			8
#define FAN_LUT_HYST_DEF		4
#define FAN_LUT_HYST_MIN		0
#define FAN_LUT_HYST_MAX		31
#define FAN_MIN_READ			0xffff
#define FAN_RPM_FACTOR			5400000

#define MANUFACTURER_ID			0x5d

#define TEMP_EXT_HI_FAULT		0x7f
#define TEMP_EXT_LO_FAULT_OPEN		0x00
#define TEMP_EXT_LO_FAULT_SHORT		0xe0

#define TEMP_LO_FRAC			125
#define TEMP_LO_SHIFT			5
#define TEMP_LO_MASK			(0x3 << TEMP_LO_SHIFT)

#define TEMP_MIN			-64
#define TEMP_MAX			127
#define TEMP_MAX_FRAC			750

enum emc2101_fan_spin_up_abort {
	EMC2101_FAN_SPIN_ABORT_DISABLE = 0,
	EMC2101_FAN_SPIN_ABORT_ENABLE
};

enum emc2101_fan_standby {
	EMC2101_FAN_STBY_DISABLE = 0,
	EMC2101_FAN_STBY_ENABLE
};

enum emc2101_mode {
	EMC2101_MODE_PWM = 0,
	EMC2101_MODE_DAC
};

enum ecm2101_product_id {
	EMC2101 = 0x16,
	EMC2101_R = 0x28
};

enum emc2101_pwm_enable {
	EMC2101_PWM_MANUAL = 1,
	EMC2101_PWM_LUT = 2
};

enum emc2101_pwm_polarity {
	EMC2101_POL_NORMAL = 0,
	EMC2101_POL_INVERTED
};

enum emc2101_temp_channels {
	EMC2101_TC_INT = 0,
	EMC2101_TC_EXT,
	EMC2101_TC_FORCE,
	EMC2101_TC_NUM
};

enum emc2101_temp_diode {
	EMC2101_TD_CPU = 1,
	EMC2101_TD_2N3904 = 2
};

struct emc2101_data {
	struct i2c_client *client;
	struct device *dev;
	struct mutex mutex;
};

static const u16 emc2101_conv_time[] = {
	62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000
};

static const u8 emc2101_fan_spin_up_power[] = {
	0, 50, 75, 100
};

static const u16 emc2101_fan_spin_up_time[] = {
	0, 50, 100, 200, 400, 800, 1600, 3200
};

static inline int emc2101_read_u8(struct emc2101_data *data, u8 reg, u8 *val)
{
	struct i2c_client *client = data->client;
	struct device *dev = data->dev;
	s32 read_byte;
	int ret;

	read_byte = i2c_smbus_read_byte_data(client, reg);
	if (read_byte < 0) {
		ret = read_byte;
		dev_err(dev, "error reading reg=%x err=%d", reg, read_byte);
	} else {
		ret = 0;
		*val = read_byte;
	}

	return ret;
}

static inline int emc2101_read_u16(struct emc2101_data *data, u8 reg_hi, u8 reg_lo, u16 *val)
{
	u8 val_hi, val_lo;
	int ret;

	ret = emc2101_read_u8(data, reg_lo, &val_lo);
	if (ret)
		return ret;

	ret = emc2101_read_u8(data, reg_hi, &val_hi);
	if (ret)
		return ret;

	*val = (val_hi) << 8;
	*val |= val_lo;

	return ret;
}

static inline int emc2101_write_u8(struct emc2101_data *data, u8 reg, u8 val)
{
	struct i2c_client *client = data->client;
	struct device *dev = data->dev;
	s32 write_byte;

	write_byte = i2c_smbus_write_byte_data(client, reg, val);
	if (write_byte < 0)
		dev_err(dev, "error writing reg=%x err=%d", reg, write_byte);

	return write_byte;
}

static inline int emc2101_write_u16(struct emc2101_data *data, u8 reg_hi, u8 reg_lo, u16 val)
{
	const u8 val_hi = (val >> 8) & 0xff;
	const u8 val_lo = val & 0xff;
	int ret;

	ret = emc2101_write_u8(data, reg_lo, val_lo);
	if (ret)
		return ret;

	return emc2101_write_u8(data, reg_hi, val_hi);
}

static inline int emc2101_rmw_u8(struct emc2101_data *data, u8 reg, u8 clr, u8 set)
{
	int ret;
	u8 val;

	ret = emc2101_read_u8(data, reg, &val);
	if (ret)
		return ret;

	val &= ~clr;
	val |= set;

	return emc2101_write_u8(data, reg, val);
}

static inline int emc2101_check_u8(struct emc2101_data *data, u8 reg, u8 mask, bool *val)
{
	u8 reg_val;
	int ret;

	ret = emc2101_read_u8(data, reg, &reg_val);
	if (!ret)
		*val = (reg_val & mask) == mask;

	return ret;
}

static int emc2101_lut_hyst_write(struct emc2101_data *data, u8 reg, long temp)
{
	const u8 val = clamp_val(temp / 1000, FAN_LUT_HYST_MIN, FAN_LUT_HYST_MAX);

	return emc2101_write_u8(data, reg, val);
}

static int emc2101_temp_read(struct emc2101_data *data, u8 reg, long *temp)
{
	int ret;
	u8 val;

	ret = emc2101_read_u8(data, reg, &val);
	if (!ret)
		*temp = val * 1000;

	return ret;
}

static int emc2101_temp_neg_write(struct emc2101_data *data, u8 reg, long temp)
{
	const u8 val = clamp_val(temp / 1000, TEMP_MIN, TEMP_MAX);

	return emc2101_write_u8(data, reg, val);
}

static int emc2101_temp_pos_write(struct emc2101_data *data, u8 reg, long temp)
{
	const u8 val = clamp_val(temp / 1000, 0, TEMP_MAX);

	return emc2101_write_u8(data, reg, val);
}

static int emc2101_temp_frac_read(struct emc2101_data *data, u8 reg_hi, u8 reg_lo, long *temp)
{
	u8 val_hi, val_lo;
	int ret;

	ret = emc2101_read_u8(data, reg_hi, &val_hi);
	if (ret)
		return ret;

	ret = emc2101_read_u8(data, reg_lo, &val_lo);
	if (ret)
		return ret;

	val_lo &= TEMP_LO_MASK;

	if (val_hi == TEMP_EXT_HI_FAULT) {
		switch (val_lo) {
		case TEMP_EXT_LO_FAULT_OPEN:
			ret = -ENODATA;
			dev_warn(data->dev, "[%02x, %02x]: diode fault (open)", reg_hi, reg_lo);
			break;
		case TEMP_EXT_LO_FAULT_SHORT:
			ret = -ENODATA;
			dev_warn(data->dev, "[%02x, %02x]: diode fault (short)", reg_hi, reg_lo);
			break;
		default:
			break;
		}
	}

	if (!ret) {
		*temp = val_lo >> TEMP_LO_SHIFT;
		*temp *= TEMP_LO_FRAC;
		*temp += val_hi * 1000;
	}

	return ret;
}

static int emc2101_temp_frac_write(struct emc2101_data *data, u8 reg_hi, u8 reg_lo, long temp)
{
	const u8 val_hi = clamp_val(temp / 1000, TEMP_MIN, TEMP_MAX);
	long fraction = temp - (val_hi * 1000);
	u8 val_lo;
	int ret;

	if (val_hi == TEMP_MAX && fraction > TEMP_MAX_FRAC)
		fraction = clamp_val(fraction, 0, TEMP_MAX_FRAC);
	fraction /= TEMP_LO_FRAC;
	val_lo = (fraction << TEMP_LO_SHIFT) & TEMP_LO_MASK;

	ret = emc2101_write_u8(data, reg_hi, val_hi);
	if (ret)
		return ret;

	return emc2101_write_u8(data, reg_lo, val_lo);
}

static int emc2101_pwm_read(struct emc2101_data *data, u8 reg, long *pwm)
{
	int ret;
	u8 val;

	ret = emc2101_read_u8(data, reg, &val);
	if (!ret)
		*pwm = val;

	return ret;
}

static int emc2101_pwm_write(struct emc2101_data *data, u8 reg, long pwm)
{
	const u8 val = clamp_val(pwm, 1, PWM_FREQ_MASK);

	return emc2101_write_u8(data, reg, val);
}

static ssize_t fan_spin_up_abort_show(struct device *dev, struct device_attribute *devattr,
				      char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool spin_abort_enabled;
	u8 fan_spin_abort;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_FAN_SPIN, FAN_SPIN_UP_ABORT, &spin_abort_enabled);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (spin_abort_enabled)
		fan_spin_abort = EMC2101_FAN_SPIN_ABORT_ENABLE;
	else
		fan_spin_abort = EMC2101_FAN_SPIN_ABORT_DISABLE;

	return sprintf(buf, "%u\n", fan_spin_abort);
}

static ssize_t fan_spin_up_abort_store(struct device *dev, struct device_attribute *devattr,
				       const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long fan_spin_abort;
	u8 clr, set;
	int ret;

	ret = kstrtol(buf, 10, &fan_spin_abort);
	if (ret)
		return ret;

	switch (fan_spin_abort) {
	case EMC2101_FAN_SPIN_ABORT_DISABLE:
		clr = FAN_SPIN_UP_ABORT;
		set = 0;
		break;
	case EMC2101_FAN_SPIN_ABORT_ENABLE:
		clr = 0;
		set = FAN_SPIN_UP_ABORT;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_SPIN, clr, set);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t fan_spin_up_time_show(struct device *dev, struct device_attribute *devattr,
				     char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 fan_spin_time;
	u8 fan_spin;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_FAN_SPIN, &fan_spin);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	fan_spin_time = emc2101_fan_spin_up_time[fan_spin & FAN_SPIN_UP_TIME_MASK];

	return sprintf(buf, "%u\n", fan_spin_time);
}

static ssize_t fan_spin_up_time_store(struct device *dev, struct device_attribute *devattr,
				      const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 fan_spin_time_idx;
	long fan_spin_time;
	int ret;

	ret = kstrtol(buf, 10, &fan_spin_time);
	if (ret)
		return ret;

	fan_spin_time_idx = find_closest(fan_spin_time, emc2101_fan_spin_up_time,
					 ARRAY_SIZE(emc2101_fan_spin_up_time));

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_SPIN, FAN_SPIN_UP_TIME_MASK, fan_spin_time_idx);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t fan_spin_up_power_show(struct device *dev, struct device_attribute *devattr,
				      char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 fan_spin_power;
	u8 fan_spin;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_FAN_SPIN, &fan_spin);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	fan_spin_power = emc2101_fan_spin_up_power[(fan_spin & FAN_SPIN_UP_POWER_MASK) >>
						   FAN_SPIN_UP_POWER_SHIFT];

	return sprintf(buf, "%u\n", fan_spin_power);
}

static ssize_t fan_spin_up_power_store(struct device *dev, struct device_attribute *devattr,
				       const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 fan_spin_power_idx;
	long fan_spin_power;
	int ret;

	ret = kstrtol(buf, 10, &fan_spin_power);
	if (ret)
		return ret;

	fan_spin_power_idx = find_closest(fan_spin_power, emc2101_fan_spin_up_power,
					  ARRAY_SIZE(emc2101_fan_spin_up_power));

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_SPIN, FAN_SPIN_UP_POWER_MASK,
			     fan_spin_power_idx << FAN_SPIN_UP_POWER_SHIFT);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t fan_standby_show(struct device *dev, struct device_attribute *devattr,
				char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool standby_enabled;
	u8 fan_standby;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_CONFIG, FAN_STANDBY_ENABLE, &standby_enabled);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (standby_enabled)
		fan_standby = EMC2101_FAN_STBY_ENABLE;
	else
		fan_standby = EMC2101_FAN_STBY_DISABLE;

	return sprintf(buf, "%u\n", fan_standby);
}

static ssize_t fan_standby_store(struct device *dev, struct device_attribute *devattr,
				 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long fan_standby;
	u8 clr, set;
	int ret;

	ret = kstrtol(buf, 10, &fan_standby);
	if (ret)
		return ret;

	switch (fan_standby) {
	case EMC2101_FAN_STBY_DISABLE:
		clr = FAN_STANDBY_ENABLE;
		set = 0;
		break;
	case EMC2101_FAN_STBY_ENABLE:
		clr = 0;
		set = FAN_STANDBY_ENABLE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_CONFIG, clr, set);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_pwm_show(struct device *dev, struct device_attribute *devattr,
				       char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct emc2101_data *data = dev_get_drvdata(dev);
	const u8 reg = REG_FAN_LUT_SPEED(attr->index);
	long lut_pwm;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_pwm_read(data, reg, &lut_pwm);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", lut_pwm);
}

static ssize_t pwm_auto_point_pwm_store(struct device *dev, struct device_attribute *devattr,
					const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct emc2101_data *data = dev_get_drvdata(dev);
	const u8 reg = REG_FAN_LUT_SPEED(attr->index);
	long lut_pwm;
	int ret;

	ret = kstrtol(buf, 10, &lut_pwm);
	if (ret)
		return ret;

	mutex_lock(&data->mutex);
	ret = emc2101_pwm_write(data, reg, lut_pwm);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_temp_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct emc2101_data *data = dev_get_drvdata(dev);
	const u8 reg = REG_FAN_LUT_TEMP(attr->index);
	long lut_temp;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, reg, &lut_temp);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", lut_temp);
}

static ssize_t __pwm_auto_point_temp_store(struct emc2101_data *data,
					   struct device_attribute *devattr, const char *buf,
					   size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	long cur_temp, lut_temp;
	bool lut_disable;
	unsigned int i;
	int ret;

	ret = kstrtol(buf, 10, &lut_temp);
	if (ret)
		return ret;

	ret = emc2101_check_u8(data, REG_FAN_CONFIG, FAN_LUT_DISABLE, &lut_disable);
	if (ret)
		return ret;

	if (!lut_disable) {
		ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, 0, FAN_LUT_DISABLE);
		if (ret)
			return ret;
	}

	for (i = 0; i < FAN_LUT_COUNT; i++) {
		const u8 reg = REG_FAN_LUT_TEMP(attr->index);

		ret = emc2101_temp_read(data, reg, &cur_temp);
		if (ret)
			return ret;

		if (i < attr->index) {
			if (cur_temp > lut_temp)
				ret = emc2101_temp_pos_write(data, reg, lut_temp);
		} else if (i > attr->index) {
			if (cur_temp < lut_temp)
				ret = emc2101_temp_pos_write(data, reg, lut_temp);
		} else {
			ret = emc2101_temp_pos_write(data, reg, lut_temp);
		}

		if (ret)
			return ret;
	}

	if (!lut_disable) {
		ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, FAN_LUT_DISABLE, 0);
		if (ret)
			return ret;
	}

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_temp_store(struct device *dev, struct device_attribute *devattr,
					 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = __pwm_auto_point_temp_store(data, devattr, buf, count);
	mutex_unlock(&data->mutex);

	return ret;
}

static ssize_t pwm_auto_point_temp_hyst_show(struct device *dev, struct device_attribute *devattr,
					     char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long lut_hyst;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, REG_FAN_LUT_HYST, &lut_hyst);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", lut_hyst);
}

static ssize_t pwm_auto_point_temp_hyst_store(struct device *dev, struct device_attribute *devattr,
					      const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long lut_hyst;
	int ret;

	ret = kstrtol(buf, 10, &lut_hyst);
	if (ret)
		return ret;

	mutex_lock(&data->mutex);
	ret = emc2101_lut_hyst_write(data, REG_FAN_LUT_HYST, lut_hyst);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t pwm_polarity_invert_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool polarity_inverted;
	u8 polarity;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_FAN_CONFIG, FAN_POL_INV, &polarity_inverted);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (polarity_inverted)
		polarity = EMC2101_POL_INVERTED;
	else
		polarity = EMC2101_POL_NORMAL;

	return sprintf(buf, "%u\n", polarity);
}

static ssize_t pwm_polarity_invert_store(struct device *dev, struct device_attribute *devattr,
					 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long polarity;
	u8 clr, set;
	int ret;

	ret = kstrtol(buf, 10, &polarity);
	if (ret)
		return ret;

	switch (polarity) {
	case EMC2101_POL_INVERTED:
		clr = 0;
		set = FAN_POL_INV;
		break;
	case EMC2101_POL_NORMAL:
		clr = FAN_POL_INV;
		set = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, clr, set);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t temp_external_force_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long temp_force;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, REG_TEMP_EXT_FORCE, &temp_force);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", temp_force);
}

static ssize_t temp_external_force_store(struct device *dev, struct device_attribute *devattr,
					 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long temp_force;
	int ret;

	ret = kstrtol(buf, 10, &temp_force);
	if (ret)
		return ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_neg_write(data, REG_TEMP_EXT_FORCE, temp_force);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t temp_external_ideality_show(struct device *dev, struct device_attribute *devattr,
					   char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long ext_ideality_factor;
	u8 ext_ideality;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_EXT_IDEALITY, &ext_ideality);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	ext_ideality_factor = EXT_IDEALITY_VAL(ext_ideality);

	return sprintf(buf, "%lu\n", ext_ideality_factor);
}

static ssize_t temp_external_ideality_store(struct device *dev, struct device_attribute *devattr,
					    const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long ext_ideality_factor;
	u8 ext_ideality;
	int ret;

	ret = kstrtol(buf, 10, &ext_ideality_factor);
	if (ret)
		return ret;

	ext_ideality_factor = clamp_val(ext_ideality_factor, EXT_IDEALITY_START,
					EXT_IDEALITY_VAL(EXT_IDEALITY_MASK));
	ext_ideality = (ext_ideality_factor - EXT_IDEALITY_START) / EXT_IDEALITY_STEP;

	mutex_lock(&data->mutex);
	ret = emc2101_write_u8(data, REG_EXT_IDEALITY, ext_ideality);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static SENSOR_DEVICE_ATTR_RW(fan1_spin_up_abort, fan_spin_up_abort, 0);
static SENSOR_DEVICE_ATTR_RW(fan1_spin_up_power, fan_spin_up_power, 0);
static SENSOR_DEVICE_ATTR_RW(fan1_spin_up_time, fan_spin_up_time, 0);
static SENSOR_DEVICE_ATTR_RW(fan1_standby, fan_standby, 0);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_pwm, pwm_auto_point_pwm, 0);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_temp, pwm_auto_point_temp, 0);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_pwm, pwm_auto_point_pwm, 1);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_temp, pwm_auto_point_temp, 1);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_pwm, pwm_auto_point_pwm, 2);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_temp, pwm_auto_point_temp, 2);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_pwm, pwm_auto_point_pwm, 3);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_temp, pwm_auto_point_temp, 3);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point5_pwm, pwm_auto_point_pwm, 4);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point5_temp, pwm_auto_point_temp, 4);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point6_pwm, pwm_auto_point_pwm, 5);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point6_temp, pwm_auto_point_temp, 5);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point7_pwm, pwm_auto_point_pwm, 6);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point7_temp, pwm_auto_point_temp, 6);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point8_pwm, pwm_auto_point_pwm, 7);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point8_temp, pwm_auto_point_temp, 7);

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point_temp_hyst, pwm_auto_point_temp_hyst, 0);

static SENSOR_DEVICE_ATTR_RW(pwm1_polarity_invert, pwm_polarity_invert, 0);

static SENSOR_DEVICE_ATTR_RW(temp2_external_ideality, temp_external_ideality, 0);

static SENSOR_DEVICE_ATTR_RW(temp3, temp_external_force, 0);

static struct attribute *emc2101_hwmon_attributes[] = {
	&sensor_dev_attr_fan1_spin_up_abort.dev_attr.attr,
	&sensor_dev_attr_fan1_spin_up_power.dev_attr.attr,
	&sensor_dev_attr_fan1_spin_up_time.dev_attr.attr,
	&sensor_dev_attr_fan1_standby.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point_temp_hyst.dev_attr.attr,
	&sensor_dev_attr_pwm1_polarity_invert.dev_attr.attr,
	&sensor_dev_attr_temp2_external_ideality.dev_attr.attr,
	&sensor_dev_attr_temp3.dev_attr.attr,
	NULL
};

static const struct attribute_group emc2101_hwmon_group = {
	.attrs = emc2101_hwmon_attributes,
};
__ATTRIBUTE_GROUPS(emc2101_hwmon);

static int emc2101_chip_update_interval_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 conv_rate;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_CONV_RATE, &conv_rate);
	mutex_unlock(&data->mutex);

	if (!ret) {
		conv_rate &= CONV_RATE_MASK;
		if (conv_rate < ARRAY_SIZE(emc2101_conv_time))
			*val = emc2101_conv_time[conv_rate];
		else
			*val = 32000;
	}

	return ret;
}

static int emc2101_chip_update_interval_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 conv_rate;
	int ret;

	conv_rate = find_closest(val, emc2101_conv_time, ARRAY_SIZE(emc2101_conv_time));

	mutex_lock(&data->mutex);
	ret = emc2101_write_u8(data, REG_CONV_RATE, conv_rate);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_fan_div_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 pwm_freq_div;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_PWM_FREQ_DIV, &pwm_freq_div);
	mutex_unlock(&data->mutex);

	if (!ret)
		*val = pwm_freq_div;

	return ret;
}

static int emc2101_fan_div_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 pwm_freq_div = val;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_write_u8(data, REG_PWM_FREQ_DIV, pwm_freq_div);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_fan_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count;
	int ret;

	ret = emc2101_read_u16(data, REG_TACH_HI, REG_TACH_LO, &tach_count);
	if (ret)
		return ret;

	if (tach_count && tach_count != FAN_MIN_READ)
		*val = FAN_RPM_FACTOR / tach_count;
	else
		*val = 0;

	return ret;
}

static int emc2101_fan_min_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u16(data, REG_TACH_MIN_HI, REG_TACH_MIN_LO, &tach_count);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (tach_count && tach_count != FAN_MIN_READ)
		*val = FAN_RPM_FACTOR / tach_count;
	else
		*val = 0;

	return ret;
}

static int emc2101_fan_min_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count;
	int ret;

	if (val)
		tach_count = clamp_val(FAN_RPM_FACTOR / val, 0, FAN_MIN_READ);
	else
		tach_count = FAN_MIN_READ;

	mutex_lock(&data->mutex);
	ret = emc2101_write_u16(data, REG_TACH_MIN_HI, REG_TACH_MIN_LO, tach_count);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_fan_min_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool tach_low;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TACH_LOW, &tach_low);
	if (!ret)
		*val = tach_low;

	return ret;
}

static int emc2101_pwm_auto_channels_temp_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 fan_config;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_FAN_CONFIG, &fan_config);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (fan_config & FAN_EXT_TEMP_FORCE)
		*val = 3;
	else
		*val = 2;

	return ret;
}

static int emc2101_pwm_auto_channels_temp_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 clr, set;
	int ret;

	switch (val) {
	case 2:
		clr = FAN_EXT_TEMP_FORCE;
		set = 0;
		break;
	case 3:
		clr = 0;
		set = FAN_EXT_TEMP_FORCE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, clr, set);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_pwm_enable_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool lut_disable;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_FAN_CONFIG, FAN_LUT_DISABLE, &lut_disable);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (lut_disable)
		*val = EMC2101_PWM_MANUAL;
	else
		*val = EMC2101_PWM_LUT;

	return ret;
}

static int emc2101_pwm_enable_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 clr, set;
	int ret;

	switch (val) {
	case EMC2101_PWM_MANUAL:
		clr = 0;
		set = FAN_LUT_DISABLE;
		break;
	case EMC2101_PWM_LUT:
		clr = FAN_LUT_DISABLE;
		set = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, clr, set);
	mutex_unlock(&data->mutex);

	return ret;
}

static int __emc2101_pwm_freq_read(struct emc2101_data *data, long *val)
{
	u8 fan_config, pwm_freq, pwm_freq_div;
	unsigned int base_clk, div;
	int ret;

	ret = emc2101_read_u8(data, REG_FAN_CONFIG, &fan_config);
	if (ret)
		return ret;

	ret = emc2101_read_u8(data, REG_PWM_FREQ, &pwm_freq);
	if (ret)
		return ret;

	if (fan_config & FAN_CLK_OVR) {
		ret = emc2101_read_u8(data, REG_PWM_FREQ_DIV, &pwm_freq_div);
		if (ret)
			return ret;
	} else {
		pwm_freq_div = 1;
	}

	if (fan_config & FAN_CLK_SEL)
		base_clk = CLK_FREQ_ALT;
	else
		base_clk = CLK_FREQ_BASE;

	div = 2 * pwm_freq * pwm_freq_div;
	if (div)
		*val = base_clk / div;
	else
		*val = 0;

	return ret;
}

static int emc2101_pwm_freq_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = __emc2101_pwm_freq_read(data, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int __emc2101_pwm_freq_write(struct emc2101_data *data, long val)
{
	u8 fan_config, pwm_freq, pwm_freq_div;
	unsigned int base_clk;
	int ret;

	ret = emc2101_read_u8(data, REG_FAN_CONFIG, &fan_config);
	if (ret)
		return ret;

	if (fan_config & FAN_CLK_OVR) {
		ret = emc2101_read_u8(data, REG_PWM_FREQ_DIV, &pwm_freq_div);
		if (ret)
			return ret;
	} else {
		pwm_freq_div = 1;
	}

	if (fan_config & FAN_CLK_SEL)
		base_clk = CLK_FREQ_ALT;
	else
		base_clk = CLK_FREQ_BASE;

	pwm_freq = base_clk / (2 * pwm_freq_div * val);

	return emc2101_pwm_write(data, REG_PWM_FREQ, pwm_freq);
}

static int emc2101_pwm_freq_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = __emc2101_pwm_freq_write(data, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_pwm_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 fan_set;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_read_u8(data, REG_FAN_SET, &fan_set);
	mutex_unlock(&data->mutex);

	if (!ret)
		*val = fan_set;

	return ret;
}

static int emc2101_pwm_input_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 fan_set = clamp_val(val, 0, FAN_SET_MASK);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_write_u8(data, REG_FAN_SET, fan_set);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_pwm_mode_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool mode_dac;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_CONFIG, FAN_MODE_DAC, &mode_dac);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (mode_dac)
		*val = EMC2101_MODE_DAC;
	else
		*val = EMC2101_MODE_PWM;

	return ret;
}

static int emc2101_pwm_mode_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 clr, set;
	int ret;

	switch (val) {
	case EMC2101_MODE_DAC:
		clr = 0;
		set = FAN_MODE_DAC;
		break;
	case EMC2101_MODE_PWM:
		clr = FAN_MODE_DAC;
		set = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_CONFIG, clr, set);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_crit_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool temp_crit;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TEMP_EXT_CRIT, &temp_crit);
	if (!ret)
		*val = temp_crit;

	return ret;
}

static int emc2101_temp_ext_crit_hyst_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, REG_TEMP_EXT_CRIT_HYST, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_crit_hyst_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_pos_write(data, REG_TEMP_EXT_CRIT_HYST, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_crit_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, REG_TEMP_EXT_CRIT, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int __emc2101_temp_ext_crit_write(struct emc2101_data *data, long val)
{
	struct device *dev = data->dev;
	bool temp_unlock;
	int ret;

	ret = emc2101_check_u8(data, REG_CONFIG, TEMP_EXT_CRIT_UNLOCK, &temp_unlock);
	if (ret)
		return ret;

	if (temp_unlock) {
		dev_err(dev, "critical temperature can only be updated once");
		return -EIO;
	}

	ret = emc2101_rmw_u8(data, REG_CONFIG, 0, TEMP_EXT_CRIT_UNLOCK);
	if (ret)
		return ret;

	return emc2101_temp_pos_write(data, REG_TEMP_EXT_CRIT, val);
}

static int emc2101_temp_ext_crit_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = __emc2101_temp_ext_crit_write(data, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_fault_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool temp_fault;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TEMP_EXT_FAULT, &temp_fault);
	if (!ret)
		*val = temp_fault;

	return ret;
}

static int emc2101_temp_ext_max_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool temp_high;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TEMP_EXT_HIGH, &temp_high);
	if (!ret)
		*val = temp_high;

	return ret;
}

static int emc2101_temp_ext_max_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_frac_read(data, REG_TEMP_EXT_MAX_HI, REG_TEMP_EXT_MAX_LO, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_max_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_frac_write(data, REG_TEMP_EXT_MAX_HI, REG_TEMP_EXT_MAX_LO, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_min_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool temp_low;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TEMP_EXT_LOW, &temp_low);
	if (!ret)
		*val = temp_low;

	return ret;
}

static int emc2101_temp_ext_min_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_frac_read(data, REG_TEMP_EXT_MIN_HI, REG_TEMP_EXT_MIN_LO, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_min_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_frac_write(data, REG_TEMP_EXT_MIN_HI, REG_TEMP_EXT_MIN_LO, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_type_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool beta_comp_disable;
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_check_u8(data, REG_BETA_COMP, BETA_COMP_DISABLE, &beta_comp_disable);
	mutex_unlock(&data->mutex);

	if (ret)
		return ret;

	if (beta_comp_disable)
		*val = EMC2101_TD_2N3904;
	else
		*val = EMC2101_TD_CPU;

	return ret;
}

static int emc2101_temp_ext_type_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u8 set, clr;
	int ret;

	switch (val) {
	case EMC2101_TD_CPU:
		clr = BETA_COMP_MASK;
		set = BETA_COMP_AUTO;
		break;
	case EMC2101_TD_2N3904:
		clr = BETA_COMP_AUTO | BETA_COMP_MASK;
		set = BETA_COMP_DISABLE;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->mutex);
	ret = emc2101_rmw_u8(data, REG_BETA_COMP, clr, set);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_ext_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_read(data, REG_TEMP_EXT_HI, REG_TEMP_EXT_LO, val);
}

static int emc2101_temp_int_max_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	bool temp_high;
	int ret;

	ret = emc2101_check_u8(data, REG_STATUS, TEMP_INT_HIGH, &temp_high);
	if (!ret)
		*val = temp_high;

	return ret;
}

static int emc2101_temp_int_max_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_read(data, REG_TEMP_INT_MAX, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_int_max_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->mutex);
	ret = emc2101_temp_pos_write(data, REG_TEMP_INT_MAX, val);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_temp_int_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_read(data, REG_TEMP_INT, val);
}

static umode_t emc2101_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	int max_channels;

	if (type == hwmon_temp)
		max_channels = EMC2101_TC_NUM;
	else
		max_channels = 1;

	if (channel >= max_channels)
		return 0;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_min_alarm:
			return 0444;
		case hwmon_fan_div:
		case hwmon_fan_min:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_auto_channels_temp:
		case hwmon_pwm_enable:
		case hwmon_pwm_freq:
		case hwmon_pwm_input:
		case hwmon_pwm_mode:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_crit_alarm:
		case hwmon_temp_fault:
		case hwmon_temp_input:
		case hwmon_temp_label:
		case hwmon_temp_max_alarm:
		case hwmon_temp_min_alarm:
			return 0444;
		case hwmon_temp_crit:
		case hwmon_temp_crit_hyst:
		case hwmon_temp_max:
		case hwmon_temp_min:
		case hwmon_temp_type:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
};

static int emc2101_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			long *val)
{
	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return emc2101_chip_update_interval_read(dev, val);
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_div:
			return emc2101_fan_div_read(dev, val);
		case hwmon_fan_input:
			return emc2101_fan_input_read(dev, val);
		case hwmon_fan_min:
			return emc2101_fan_min_read(dev, val);
		case hwmon_fan_min_alarm:
			return emc2101_fan_min_alarm_read(dev, val);
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_auto_channels_temp:
			return emc2101_pwm_auto_channels_temp_read(dev, val);
		case hwmon_pwm_enable:
			return emc2101_pwm_enable_read(dev, val);
		case hwmon_pwm_freq:
			return emc2101_pwm_freq_read(dev, val);
		case hwmon_pwm_input:
			return emc2101_pwm_input_read(dev, val);
		case hwmon_pwm_mode:
			return emc2101_pwm_mode_read(dev, val);
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_crit:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_crit_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_crit_alarm:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_crit_alarm_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_crit_hyst:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_crit_hyst_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_fault:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_fault_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_input:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_input_read(dev, val);
			case EMC2101_TC_INT:
				return emc2101_temp_int_input_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_max:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_max_read(dev, val);
			case EMC2101_TC_INT:
				return emc2101_temp_int_max_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_max_alarm:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_max_alarm_read(dev, val);
			case EMC2101_TC_INT:
				return emc2101_temp_int_max_alarm_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_min:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_min_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_min_alarm:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_min_alarm_read(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_type:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_type_read(dev, val);
			default:
				break;
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
};

static int emc2101_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			switch (channel) {
			case EMC2101_TC_EXT:
				*str = "external";
				return 0;
			case EMC2101_TC_FORCE:
				*str = "force";
				return 0;
			case EMC2101_TC_INT:
				*str = "internal";
				return 0;
			default:
				break;
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
};

static int emc2101_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			 long val)
{
	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return emc2101_chip_update_interval_write(dev, val);
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_div:
			return emc2101_fan_div_write(dev, val);
		case hwmon_fan_min:
			return emc2101_fan_min_write(dev, val);
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_auto_channels_temp:
			return emc2101_pwm_auto_channels_temp_write(dev, val);
		case hwmon_pwm_enable:
			return emc2101_pwm_enable_write(dev, val);
		case hwmon_pwm_freq:
			return emc2101_pwm_freq_write(dev, val);
		case hwmon_pwm_input:
			return emc2101_pwm_input_write(dev, val);
		case hwmon_pwm_mode:
			return emc2101_pwm_mode_write(dev, val);
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_crit:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_crit_write(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_crit_hyst:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_crit_hyst_write(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_max:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_max_write(dev, val);
			case EMC2101_TC_INT:
				return emc2101_temp_int_max_write(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_min:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_min_write(dev, val);
			default:
				break;
			}
			break;
		case hwmon_temp_type:
			switch (channel) {
			case EMC2101_TC_EXT:
				return emc2101_temp_ext_type_write(dev, val);
			default:
				break;
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

#define EMC2101_CHIP_CFG	HWMON_C_UPDATE_INTERVAL
#define EMC2101_FAN_CFG		(HWMON_F_DIV |\
				 HWMON_F_INPUT |\
				 HWMON_F_MIN |\
				 HWMON_F_MIN_ALARM)
#define EMC2101_PWM_CFG		(HWMON_PWM_AUTO_CHANNELS_TEMP |\
				 HWMON_PWM_ENABLE |\
				 HWMON_PWM_FREQ |\
				 HWMON_PWM_INPUT |\
				 HWMON_PWM_MODE)
#define EMC2101_TEMP_INT_CFG	(HWMON_T_INPUT |\
				 HWMON_T_LABEL |\
				 HWMON_T_MAX |\
				 HWMON_T_MAX_ALARM)
#define EMC2101_TEMP_EXT_CFG	(HWMON_T_CRIT |\
				 HWMON_T_CRIT_ALARM |\
				 HWMON_T_CRIT_HYST |\
				 HWMON_T_FAULT |\
				 HWMON_T_INPUT |\
				 HWMON_T_LABEL |\
				 HWMON_T_MAX |\
				 HWMON_T_MAX_ALARM |\
				 HWMON_T_MIN |\
				 HWMON_T_MIN_ALARM |\
				 HWMON_T_TYPE)
#define EMC2101_TEMP_FORCE_CFG	HWMON_T_LABEL

static const struct hwmon_channel_info * const emc2101_info[] = {
	HWMON_CHANNEL_INFO(chip, EMC2101_CHIP_CFG),
	HWMON_CHANNEL_INFO(fan, EMC2101_FAN_CFG),
	HWMON_CHANNEL_INFO(pwm, EMC2101_PWM_CFG),
	HWMON_CHANNEL_INFO(temp, EMC2101_TEMP_INT_CFG,
			   EMC2101_TEMP_EXT_CFG,
			   EMC2101_TEMP_FORCE_CFG),
	NULL
};

static const struct hwmon_ops emc2101_ops = {
	.is_visible = emc2101_is_visible,
	.read = emc2101_read,
	.read_string = emc2101_read_string,
	.write = emc2101_write,
};

static const struct hwmon_chip_info emc2101_chip_info = {
	.info = emc2101_info,
	.ops = &emc2101_ops,
};

static int emc2101_init(struct emc2101_data *data)
{
	static const u8 lut_t[FAN_LUT_COUNT] = {  35,   40,   45,   50,   55,   60,   70,   75};
	static const u8 lut_s[FAN_LUT_COUNT] = {0x12, 0x19, 0x1f, 0x25, 0x2c, 0x32, 0x38, 0x3f};
	unsigned int i;
	int ret;

	ret = emc2101_write_u8(data, REG_CONFIG,
			       (FAN_STANDBY_MODE |
				PIN_FUNC_TACH |
				SMBUS_TOUT_DISABLE));

	ret = emc2101_write_u8(data, REG_FAN_CONFIG,
			       (FAN_CLK_OVR |
				FAN_LUT_DISABLE |
				TACH_FALSE_READ_DISABLE));
	if (ret)
		return ret;
	for (i = 0; i < FAN_LUT_COUNT; i++) {
		ret = emc2101_write_u8(data, REG_FAN_LUT_TEMP(i), lut_t[i]);
		if (ret)
			return ret;
		ret = emc2101_write_u8(data, REG_FAN_LUT_SPEED(i), lut_s[i]);
		if (ret)
			return ret;
	}
	ret = emc2101_rmw_u8(data, REG_FAN_CONFIG, FAN_LUT_DISABLE, 0);
	if (ret)
		return ret;

	ret = emc2101_write_u8(data, REG_FAN_SPIN,
			       (FAN_SPIN_UP_POWER_100 |
				FAN_SPIN_UP_ABORT |
				FAN_SPIN_UP_TIME_3200));
	if (ret)
		return ret;

	ret = emc2101_write_u8(data, REG_FAN_LUT_HYST, FAN_LUT_HYST_DEF);
	if (ret)
		return ret;

	ret = emc2101_write_u8(data, REG_PWM_FREQ, PWM_FREQ_MASK);
	if (ret)
		return ret;

	ret = emc2101_write_u8(data, REG_PWM_FREQ_DIV, 1);
	if (ret)
		return ret;

	ret = emc2101_write_u8(data, REG_AVG_FILTER, FILTER_NONE);
	if (ret)
		return ret;

	ret = emc2101_write_u16(data, REG_TACH_MIN_HI, REG_TACH_MIN_LO, FAN_MIN_READ);
	if (ret)
		return ret;

	return emc2101_rmw_u8(data, REG_BETA_COMP, 0, BETA_COMP_DISABLE);
}

static int emc2101_probe(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	struct device *dev = &client->dev;
	struct emc2101_data *data;
	struct device *hwmon_dev;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	data = devm_kzalloc(dev, sizeof(struct emc2101_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->dev = dev;
	mutex_init(&data->mutex);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, data,
							 &emc2101_chip_info,
							 emc2101_hwmon_groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "%s: sensor '%s'\n", dev_name(hwmon_dev), client->name);

	return emc2101_init(data);
}

static int emc2101_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	s32 manufacturer, product, revision;
	struct device *dev = &adapter->dev;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	manufacturer = i2c_smbus_read_byte_data(client, REG_MANUFACTURER_ID);
	if (manufacturer != MANUFACTURER_ID)
		return -ENODEV;

	product = i2c_smbus_read_byte_data(client, REG_PRODUCT_ID);
	switch (product) {
	case EMC2101:
		strscpy(info->type, "emc2101", I2C_NAME_SIZE);
		break;
	case EMC2101_R:
		strscpy(info->type, "emc2101-r", I2C_NAME_SIZE);
		break;
	default:
		return -ENODEV;
	}

	revision = i2c_smbus_read_byte_data(client, REG_REVISION);

	dev_info(dev, "Found %s at 0x%02x (rev 0x%02x).\n",
		 info->type, client->addr, revision);

	return 0;
}

static const struct i2c_device_id emc2101_ids[] = {
	{ "emc2101" },
	{ "emc2101-r" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, emc2101_ids);

static const struct of_device_id emc2101_of_match_table[] = {
	{ .compatible = "microchip,emc2101", },
	{ .compatible = "microchip,emc2101-r", },
	{ },
};
MODULE_DEVICE_TABLE(of, emc2101_of_match_table);

static const unsigned short emc2101_address_list[] = {
	0x4c, I2C_CLIENT_END
};

static struct i2c_driver emc2101_driver = {
	.address_list = emc2101_address_list,
	.class = I2C_CLASS_HWMON,
	.detect = emc2101_detect,
	.driver = {
		.name = "emc2101",
		.of_match_table = emc2101_of_match_table,
	},
	.id_table = emc2101_ids,
	.probe = emc2101_probe,
};
module_i2c_driver(emc2101_driver);

MODULE_AUTHOR("Álvaro Fernández Rojas <noltari@gmail.com>");
MODULE_DESCRIPTION("Microchip EMC2101 hwmon driver");
MODULE_LICENSE("GPL");
