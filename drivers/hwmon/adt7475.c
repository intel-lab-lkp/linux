// SPDX-License-Identifier: GPL-2.0-only
/*
 * adt7475 - Thermal sensor driver for the ADT7475 chip and derivatives
 * Copyright (C) 2007-2008, Advanced Micro Devices, Inc.
 * Copyright (C) 2008 Jordan Crouse <jordan@cosmicpenguin.net>
 * Copyright (C) 2008 Hans de Goede <hdegoede@redhat.com>
 * Copyright (C) 2009 Jean Delvare <jdelvare@suse.de>
 *
 * Derived from the lm83 driver by Jean Delvare
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon-vid.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/util_macros.h>

#include <dt-bindings/pwm/pwm.h>

/* Indexes for the sysfs hooks */
enum adt_sysfs_id {
	INPUT		= 0,
	MIN		= 1,
	MAX		= 2,
	CONTROL		= 3,
	OFFSET		= 3,	// Dup
	AUTOMIN		= 4,
	THERM		= 5,
	HYSTERSIS	= 6,
/*
 * These are unique identifiers for the sysfs functions - unlike the
 * numbers above, these are not also indexes into an array
 */
	ALARM		= 9,
	FAULT		= 10,
};


/* 7475 Common Registers */

#define REG_DEVREV2		0x12	/* ADT7490 only */
#define REG_IMON		0x1D	/* ADT7490 only */

#define REG_VTT			0x1E	/* ADT7490 only */
#define REG_EXTEND3		0x1F	/* ADT7490 only */

#define REG_VOLTAGE_BASE	0x20
#define REG_TEMP_BASE		0x25
#define REG_TACH_BASE		0x28
#define REG_PWM_BASE		0x30
#define REG_PWM_MAX_BASE	0x38

#define REG_DEVID		0x3D
#define REG_VENDID		0x3E
#define REG_DEVID2		0x3F

#define REG_CONFIG1		0x40

#define REG_STATUS1		0x41
#define REG_STATUS2		0x42

#define REG_VID			0x43	/* ADT7476 only */

#define REG_VOLTAGE_MIN_BASE	0x44
#define REG_VOLTAGE_MAX_BASE	0x45

#define REG_TEMP_MIN_BASE	0x4E
#define REG_TEMP_MAX_BASE	0x4F

#define REG_TACH_MIN_BASE	0x54

#define REG_PWM_CONFIG_BASE	0x5C

#define REG_TEMP_TRANGE_BASE	0x5F

#define REG_ENHANCE_ACOUSTICS1	0x62
#define REG_ENHANCE_ACOUSTICS2	0x63

#define REG_PWM_MIN_BASE	0x64

#define REG_TEMP_TMIN_BASE	0x67
#define REG_TEMP_THERM_BASE	0x6A

#define REG_REMOTE1_HYSTERSIS	0x6D
#define REG_REMOTE2_HYSTERSIS	0x6E

#define REG_TEMP_OFFSET_BASE	0x70

#define REG_CONFIG2		0x73

#define REG_EXTEND1		0x76
#define REG_EXTEND2		0x77

#define REG_CONFIG3		0x78
#define REG_CONFIG5		0x7C
#define REG_CONFIG4		0x7D

#define REG_STATUS4		0x81	/* ADT7490 only */

#define REG_VTT_MIN		0x84	/* ADT7490 only */
#define REG_VTT_MAX		0x86	/* ADT7490 only */

#define REG_IMON_MIN		0x85	/* ADT7490 only */
#define REG_IMON_MAX		0x87	/* ADT7490 only */

#define VID_VIDSEL		0x80	/* ADT7476 only */

#define CONFIG2_ATTN		0x20

#define CONFIG3_SMBALERT	0x01
#define CONFIG3_THERM		0x02

#define CONFIG4_PINFUNC		0x03
#define CONFIG4_THERM		0x01
#define CONFIG4_SMBALERT	0x02
#define CONFIG4_MAXDUTY		0x08
#define CONFIG4_ATTN_IN10	0x30
#define CONFIG4_ATTN_IN43	0xC0

#define CONFIG5_TWOSCOMP	0x01
#define CONFIG5_TEMPOFFSET	0x02
#define CONFIG5_VIDGPIO		0x10	/* ADT7476 only */

/* ADT7475 Settings */

#define ADT7475_VOLTAGE_COUNT	5	/* Not counting Vtt or Imon */
#define ADT7475_TEMP_COUNT	3
#define ADT7475_TACH_COUNT	4
#define ADT7475_PWM_COUNT	3

/* Macro to read the registers */

#define adt7475_read(reg) i2c_smbus_read_byte_data(client, (reg))

/* Macros to easily index the registers */

#define TACH_REG(idx) (REG_TACH_BASE + ((idx) * 2))
#define TACH_MIN_REG(idx) (REG_TACH_MIN_BASE + ((idx) * 2))

#define PWM_REG(idx) (REG_PWM_BASE + (idx))
#define PWM_MAX_REG(idx) (REG_PWM_MAX_BASE + (idx))
#define PWM_MIN_REG(idx) (REG_PWM_MIN_BASE + (idx))
#define PWM_CONFIG_REG(idx) (REG_PWM_CONFIG_BASE + (idx))

#define VOLTAGE_REG(idx) (REG_VOLTAGE_BASE + (idx))
#define VOLTAGE_MIN_REG(idx) (REG_VOLTAGE_MIN_BASE + ((idx) * 2))
#define VOLTAGE_MAX_REG(idx) (REG_VOLTAGE_MAX_BASE + ((idx) * 2))

#define TEMP_REG(idx) (REG_TEMP_BASE + (idx))
#define TEMP_MIN_REG(idx) (REG_TEMP_MIN_BASE + ((idx) * 2))
#define TEMP_MAX_REG(idx) (REG_TEMP_MAX_BASE + ((idx) * 2))
#define TEMP_TMIN_REG(idx) (REG_TEMP_TMIN_BASE + (idx))
#define TEMP_THERM_REG(idx) (REG_TEMP_THERM_BASE + (idx))
#define TEMP_OFFSET_REG(idx) (REG_TEMP_OFFSET_BASE + (idx))
#define TEMP_TRANGE_REG(idx) (REG_TEMP_TRANGE_BASE + (idx))

static const unsigned short normal_i2c[] = { 0x2c, 0x2d, 0x2e, I2C_CLIENT_END };

enum chips { adt7473, adt7475, adt7476, adt7490 };

static const struct i2c_device_id adt7475_id[] = {
	{ .name = "adt7473", .driver_data = adt7473 },
	{ .name = "adt7475", .driver_data = adt7475 },
	{ .name = "adt7476", .driver_data = adt7476 },
	{ .name = "adt7490", .driver_data = adt7490 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adt7475_id);

static const struct of_device_id adt7475_of_match[] = {
	{
		.compatible = "adi,adt7473",
		.data = (void *)adt7473
	},
	{
		.compatible = "adi,adt7475",
		.data = (void *)adt7475
	},
	{
		.compatible = "adi,adt7476",
		.data = (void *)adt7476
	},
	{
		.compatible = "adi,adt7490",
		.data = (void *)adt7490
	},
	{ },
};
MODULE_DEVICE_TABLE(of, adt7475_of_match);

struct adt7475_data {
	struct i2c_client *client;
	struct mutex lock;

	u64 measure_updated;
	bool valid;

	u8 config2;
	u8 config4;
	u8 config5;
	u8 has_voltage;
	u8 bypass_attn;		/* Bypass voltage attenuator */
	u8 has_pwm2:1;
	u8 has_fan4:1;
	u8 has_vid:1;
	u32 alarms;
	u16 voltage[3][7];
	u16 temp[7][3];
	u16 tach[2][4];
	u8 pwm[4][3];
	u8 range[3];
	u8 pwmctl[3];
	u8 pwmchan[3];
	u8 enh_acoustics[2];
	u8 pwm_held;		/* Channels requested through the PWM framework */

	u8 vid;
	u8 vrm;
	const struct attribute_group *groups[4];
};

static struct i2c_driver adt7475_driver;
static struct adt7475_data *adt7475_update_device(struct device *dev);
static void adt7475_read_hystersis(struct i2c_client *client);
static void adt7475_read_pwm(struct i2c_client *client, int index);

/* Given a temp value, convert it to register value */

static inline u16 temp2reg(struct adt7475_data *data, long val)
{
	u16 ret;

	if (!(data->config5 & CONFIG5_TWOSCOMP)) {
		val = clamp_val(val, -64000, 191000);
		ret = (val + 64500) / 1000;
	} else {
		val = clamp_val(val, -128000, 127000);
		if (val < -500)
			ret = (256500 + val) / 1000;
		else
			ret = (val + 500) / 1000;
	}

	return ret << 2;
}

/* Given a register value, convert it to a real temp value */

static inline int reg2temp(struct adt7475_data *data, u16 reg)
{
	if (data->config5 & CONFIG5_TWOSCOMP) {
		if (reg >= 512)
			return (reg - 1024) * 250;
		else
			return reg * 250;
	} else
		return (reg - 256) * 250;
}

static inline int tach2rpm(u16 tach)
{
	if (tach == 0 || tach == 0xFFFF)
		return 0;

	return (90000 * 60) / tach;
}

static inline u16 rpm2tach(unsigned long rpm)
{
	if (rpm == 0)
		return 0;

	return clamp_val((90000 * 60) / rpm, 1, 0xFFFF);
}

/* Scaling factors for voltage inputs, taken from the ADT7490 datasheet */
static const int adt7473_in_scaling[ADT7475_VOLTAGE_COUNT + 2][2] = {
	{ 45, 94 },	/* +2.5V */
	{ 175, 525 },	/* Vccp */
	{ 68, 71 },	/* Vcc */
	{ 93, 47 },	/* +5V */
	{ 120, 20 },	/* +12V */
	{ 45, 45 },	/* Vtt */
	{ 45, 45 },	/* Imon */
};

static inline int reg2volt(int channel, u16 reg, u8 bypass_attn)
{
	const int *r = adt7473_in_scaling[channel];

	if (bypass_attn & (1 << channel))
		return DIV_ROUND_CLOSEST(reg * 2250, 1024);
	return DIV_ROUND_CLOSEST(reg * (r[0] + r[1]) * 2250, r[1] * 1024);
}

static inline u16 volt2reg(int channel, long volt, u8 bypass_attn)
{
	const int *r = adt7473_in_scaling[channel];
	long reg;

	if (bypass_attn & (1 << channel))
		reg = DIV_ROUND_CLOSEST(volt * 1024, 2250);
	else
		reg = DIV_ROUND_CLOSEST(volt * r[1] * 1024,
					(r[0] + r[1]) * 2250);
	return clamp_val(reg, 0, 1023) & (0xff << 2);
}

static int adt7475_read_word(struct i2c_client *client, int reg)
{
	int val1, val2;

	val1 = i2c_smbus_read_byte_data(client, reg);
	if (val1 < 0)
		return val1;
	val2 = i2c_smbus_read_byte_data(client, reg + 1);
	if (val2 < 0)
		return val2;

	return val1 | (val2 << 8);
}

static void adt7475_write_word(struct i2c_client *client, int reg, u16 val)
{
	i2c_smbus_write_byte_data(client, reg + 1, val >> 8);
	i2c_smbus_write_byte_data(client, reg, val & 0xFF);
}

static int adt7475_temp_value(struct adt7475_data *data, int nr, int index)
{
	int out;

	switch (nr) {
	case HYSTERSIS:
		mutex_lock(&data->lock);
		out = data->temp[nr][index];
		if (index != 1)
			out = (out >> 4) & 0xF;
		else
			out = (out & 0xF);
		/*
		 * Show the value as an absolute number tied to
		 * THERM
		 */
		out = reg2temp(data, data->temp[THERM][index]) -
			out * 1000;
		mutex_unlock(&data->lock);
		break;

	case OFFSET:
		/*
		 * Offset is always 2's complement, regardless of the
		 * setting in CONFIG5
		 */
		mutex_lock(&data->lock);
		out = (s8)data->temp[nr][index];
		if (data->config5 & CONFIG5_TEMPOFFSET)
			out *= 1000;
		else
			out *= 500;
		mutex_unlock(&data->lock);
		break;

	case ALARM:
		out = (data->alarms >> (index + 4)) & 1;
		break;

	case FAULT:
		/* Note - only for remote1 and remote2 */
		out = !!(data->alarms & (index ? 0x8000 : 0x4000));
		break;

	default:
		/* All other temp values are in the configured format */
		out = reg2temp(data, data->temp[nr][index]);
	}

	return out;
}

static ssize_t temp_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct adt7475_data *data = adt7475_update_device(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n",
		       adt7475_temp_value(data, sattr->nr, sattr->index));
}

static void adt7475_temp_set(struct adt7475_data *data, int nr, int index,
			     long val)
{
	struct i2c_client *client = data->client;
	unsigned char reg = 0;
	u8 out;
	int temp;

	mutex_lock(&data->lock);

	/* We need the config register in all cases for temp <-> reg conv. */
	data->config5 = adt7475_read(REG_CONFIG5);

	switch (nr) {
	case OFFSET:
		if (data->config5 & CONFIG5_TEMPOFFSET) {
			val = clamp_val(val, -63000, 127000);
			data->temp[OFFSET][index] = val / 1000;
		} else {
			val = clamp_val(val, -63000, 64000);
			data->temp[OFFSET][index] = val / 500;
		}
		out = data->temp[OFFSET][index];
		break;

	case HYSTERSIS:
		/*
		 * The value will be given as an absolute value, turn it
		 * into an offset based on THERM
		 */

		/* Read fresh THERM and HYSTERSIS values from the chip */
		data->temp[THERM][index] =
			adt7475_read(TEMP_THERM_REG(index)) << 2;
		adt7475_read_hystersis(client);

		temp = reg2temp(data, data->temp[THERM][index]);
		val = clamp_val(val, temp - 15000, temp);
		val = (temp - val) / 1000;

		if (index != 1) {
			data->temp[HYSTERSIS][index] &= 0x0F;
			data->temp[HYSTERSIS][index] |= (val & 0xF) << 4;
		} else {
			data->temp[HYSTERSIS][index] &= 0xF0;
			data->temp[HYSTERSIS][index] |= (val & 0xF);
		}

		out = data->temp[HYSTERSIS][index];
		break;

	default:
		data->temp[nr][index] = temp2reg(data, val);

		/*
		 * We maintain an extra 2 digits of precision for simplicity
		 * - shift those back off before writing the value
		 */
		out = (u8)(data->temp[nr][index] >> 2);
	}

	switch (nr) {
	case MIN:
		reg = TEMP_MIN_REG(index);
		break;
	case MAX:
		reg = TEMP_MAX_REG(index);
		break;
	case OFFSET:
		reg = TEMP_OFFSET_REG(index);
		break;
	case AUTOMIN:
		reg = TEMP_TMIN_REG(index);
		break;
	case THERM:
		reg = TEMP_THERM_REG(index);
		break;
	case HYSTERSIS:
		if (index != 2)
			reg = REG_REMOTE1_HYSTERSIS;
		else
			reg = REG_REMOTE2_HYSTERSIS;

		break;
	}

	i2c_smbus_write_byte_data(client, reg, out);

	mutex_unlock(&data->lock);
}

static ssize_t temp_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	adt7475_temp_set(data, sattr->nr, sattr->index, val);

	return count;
}

/* Assuming CONFIG6[SLOW] is 0 */
static const int ad7475_st_map[] = {
	37500, 18800, 12500, 7500, 4700, 3100, 1600, 800,
};

static ssize_t temp_st_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);
	long val;

	switch (sattr->index) {
	case 0:
		val = data->enh_acoustics[0] & 0xf;
		break;
	case 1:
		val = data->enh_acoustics[1] & 0xf;
		break;
	case 2:
	default:
		val = (data->enh_acoustics[1] >> 4) & 0xf;
		break;
	}

	if (val & 0x8)
		return sprintf(buf, "%d\n", ad7475_st_map[val & 0x7]);
	else
		return sprintf(buf, "0\n");
}

static ssize_t temp_st_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned char reg;
	int shift, idx;
	ulong val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	switch (sattr->index) {
	case 0:
		reg = REG_ENHANCE_ACOUSTICS1;
		shift = 0;
		idx = 0;
		break;
	case 1:
		reg = REG_ENHANCE_ACOUSTICS2;
		shift = 0;
		idx = 1;
		break;
	case 2:
	default:
		reg = REG_ENHANCE_ACOUSTICS2;
		shift = 4;
		idx = 1;
		break;
	}

	if (val > 0) {
		val = find_closest_descending(val, ad7475_st_map,
					      ARRAY_SIZE(ad7475_st_map));
		val |= 0x8;
	}

	mutex_lock(&data->lock);

	data->enh_acoustics[idx] &= ~(0xf << shift);
	data->enh_acoustics[idx] |= (val << shift);

	i2c_smbus_write_byte_data(client, reg, data->enh_acoustics[idx]);

	mutex_unlock(&data->lock);

	return count;
}

/*
 * Table of autorange values - the user will write the value in millidegrees,
 * and we'll convert it
 */
static const int autorange_table[] = {
	2000, 2500, 3330, 4000, 5000, 6670, 8000,
	10000, 13330, 16000, 20000, 26670, 32000, 40000,
	53330, 80000
};

static ssize_t point2_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct adt7475_data *data = adt7475_update_device(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int out, val;

	if (IS_ERR(data))
		return PTR_ERR(data);

	mutex_lock(&data->lock);
	out = (data->range[sattr->index] >> 4) & 0x0F;
	val = reg2temp(data, data->temp[AUTOMIN][sattr->index]);
	mutex_unlock(&data->lock);

	return sprintf(buf, "%d\n", val + autorange_table[out]);
}

static ssize_t point2_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int temp;
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	mutex_lock(&data->lock);

	/* Get a fresh copy of the needed registers */
	data->config5 = adt7475_read(REG_CONFIG5);
	data->temp[AUTOMIN][sattr->index] =
		adt7475_read(TEMP_TMIN_REG(sattr->index)) << 2;
	data->range[sattr->index] =
		adt7475_read(TEMP_TRANGE_REG(sattr->index));

	/*
	 * The user will write an absolute value, so subtract the start point
	 * to figure the range
	 */
	temp = reg2temp(data, data->temp[AUTOMIN][sattr->index]);
	val = clamp_val(val, temp + autorange_table[0],
		temp + autorange_table[ARRAY_SIZE(autorange_table) - 1]);
	val -= temp;

	/* Find the nearest table entry to what the user wrote */
	val = find_closest(val, autorange_table, ARRAY_SIZE(autorange_table));

	data->range[sattr->index] &= ~0xF0;
	data->range[sattr->index] |= val << 4;

	i2c_smbus_write_byte_data(client, TEMP_TRANGE_REG(sattr->index),
				  data->range[sattr->index]);

	mutex_unlock(&data->lock);
	return count;
}

static ssize_t pwm_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct adt7475_data *data = adt7475_update_device(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->pwm[sattr->nr][sattr->index]);
}

/*
 * Writes to the hwmon attributes of a PWM channel fail while a PWM consumer
 * holds it. Call with data->lock held.
 */
static int adt7475_pwm_check_held(struct adt7475_data *data, int index)
{
	lockdep_assert_held(&data->lock);

	return data->pwm_held & BIT(index) ? -EBUSY : 0;
}

static int adt7475_pwm_set(struct adt7475_data *data, int nr, int index,
			   long val)
{
	struct i2c_client *client = data->client;
	unsigned char reg = 0;
	int ret;

	mutex_lock(&data->lock);

	ret = adt7475_pwm_check_held(data, index);
	if (ret) {
		mutex_unlock(&data->lock);
		return ret;
	}

	switch (nr) {
	case INPUT:
		/* Get a fresh value for CONTROL */
		data->pwm[CONTROL][index] =
			adt7475_read(PWM_CONFIG_REG(index));

		/*
		 * If we are not in manual mode, then we shouldn't allow
		 * the user to set the pwm speed
		 */
		if (((data->pwm[CONTROL][index] >> 5) & 7) != 7) {
			mutex_unlock(&data->lock);
			return 0;
		}

		reg = PWM_REG(index);
		break;

	case MIN:
		reg = PWM_MIN_REG(index);
		break;

	case MAX:
		reg = PWM_MAX_REG(index);
		break;
	}

	data->pwm[nr][index] = clamp_val(val, 0, 0xFF);
	i2c_smbus_write_byte_data(client, reg, data->pwm[nr][index]);
	mutex_unlock(&data->lock);

	return 0;
}

static ssize_t pwm_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);
	long val;
	int ret;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	ret = adt7475_pwm_set(data, sattr->nr, sattr->index, val);

	return ret ? ret : count;
}

static ssize_t stall_disable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);

	u8 mask = BIT(5 + sattr->index);

	return sprintf(buf, "%d\n", !!(data->enh_acoustics[0] & mask));
}

static ssize_t stall_disable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	long val;
	u8 mask = BIT(5 + sattr->index);

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	mutex_lock(&data->lock);

	if (adt7475_pwm_check_held(data, sattr->index)) {
		mutex_unlock(&data->lock);
		return -EBUSY;
	}

	data->enh_acoustics[0] &= ~mask;
	if (val)
		data->enh_acoustics[0] |= mask;

	i2c_smbus_write_byte_data(client, REG_ENHANCE_ACOUSTICS1,
				  data->enh_acoustics[0]);

	mutex_unlock(&data->lock);

	return count;
}

/* Called by set_pwmctrl and set_pwmchan */

static int hw_set_pwm(struct i2c_client *client, int index,
		      unsigned int pwmctl, unsigned int pwmchan)
{
	struct adt7475_data *data = i2c_get_clientdata(client);
	long val = 0;

	switch (pwmctl) {
	case 0:
		val = 0x03;	/* Run at full speed */
		break;
	case 1:
		val = 0x07;	/* Manual mode */
		break;
	case 2:
		switch (pwmchan) {
		case 1:
			/* Remote1 controls PWM */
			val = 0x00;
			break;
		case 2:
			/* local controls PWM */
			val = 0x01;
			break;
		case 4:
			/* remote2 controls PWM */
			val = 0x02;
			break;
		case 6:
			/* local/remote2 control PWM */
			val = 0x05;
			break;
		case 7:
			/* All three control PWM */
			val = 0x06;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	data->pwmctl[index] = pwmctl;
	data->pwmchan[index] = pwmchan;

	data->pwm[CONTROL][index] &= ~0xE0;
	data->pwm[CONTROL][index] |= (val & 7) << 5;

	i2c_smbus_write_byte_data(client, PWM_CONFIG_REG(index),
				  data->pwm[CONTROL][index]);

	return 0;
}

/* List of frequencies for the PWM */
static const int pwmfreq_table[] = {
	11, 14, 22, 29, 35, 44, 58, 88, 22500
};

static ssize_t pwm_use_point2_pwm_at_crit_show(struct device *dev,
					       struct device_attribute *devattr,
					       char *buf)
{
	struct adt7475_data *data = adt7475_update_device(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", !!(data->config4 & CONFIG4_MAXDUTY));
}

static ssize_t pwm_use_point2_pwm_at_crit_store(struct device *dev,
						struct device_attribute *devattr,
						const char *buf, size_t count)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&data->lock);
	data->config4 = i2c_smbus_read_byte_data(client, REG_CONFIG4);
	if (val)
		data->config4 |= CONFIG4_MAXDUTY;
	else
		data->config4 &= ~CONFIG4_MAXDUTY;
	i2c_smbus_write_byte_data(client, REG_CONFIG4, data->config4);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t vrm_show(struct device *dev, struct device_attribute *devattr,
			char *buf)
{
	struct adt7475_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", (int)data->vrm);
}

static ssize_t vrm_store(struct device *dev, struct device_attribute *devattr,
			 const char *buf, size_t count)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val < 0 || val > 255)
		return -EINVAL;
	data->vrm = val;

	return count;
}

static ssize_t cpu0_vid_show(struct device *dev,
			     struct device_attribute *devattr, char *buf)
{
	struct adt7475_data *data = adt7475_update_device(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", vid_from_reg(data->vid, data->vrm));
}

/* STATUS bit of each voltage input's alarm, in alarms */
static const u8 adt7475_in_alarm_bit[] = { 0, 1, 2, 3, 8, 31, 30 };

static int adt7475_in_read(struct device *dev, u32 attr, int channel,
			   long *val)
{
	struct adt7475_data *data = adt7475_update_device(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_in_input:
		*val = reg2volt(channel, data->voltage[INPUT][channel],
				data->bypass_attn);
		return 0;
	case hwmon_in_min:
		*val = reg2volt(channel, data->voltage[MIN][channel],
				data->bypass_attn);
		return 0;
	case hwmon_in_max:
		*val = reg2volt(channel, data->voltage[MAX][channel],
				data->bypass_attn);
		return 0;
	case hwmon_in_alarm:
		*val = (data->alarms >> adt7475_in_alarm_bit[channel]) & 1;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_in_write(struct device *dev, u32 attr, int channel,
			    long val)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned char reg;
	int nr;

	switch (attr) {
	case hwmon_in_min:
		nr = MIN;
		break;
	case hwmon_in_max:
		nr = MAX;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->lock);

	data->voltage[nr][channel] = volt2reg(channel, val, data->bypass_attn);

	if (channel < ADT7475_VOLTAGE_COUNT)
		reg = nr == MIN ? VOLTAGE_MIN_REG(channel) :
				  VOLTAGE_MAX_REG(channel);
	else if (channel == 5)
		reg = nr == MIN ? REG_VTT_MIN : REG_VTT_MAX;
	else
		reg = nr == MIN ? REG_IMON_MIN : REG_IMON_MAX;

	i2c_smbus_write_byte_data(client, reg,
				  data->voltage[nr][channel] >> 2);
	mutex_unlock(&data->lock);

	return 0;
}

static int adt7475_temp_nr(u32 attr)
{
	switch (attr) {
	case hwmon_temp_input:
		return INPUT;
	case hwmon_temp_min:
		return MIN;
	case hwmon_temp_max:
		return MAX;
	case hwmon_temp_offset:
		return OFFSET;
	case hwmon_temp_crit:
		return THERM;
	case hwmon_temp_crit_hyst:
		return HYSTERSIS;
	case hwmon_temp_alarm:
		return ALARM;
	case hwmon_temp_fault:
		return FAULT;
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_temp_read(struct device *dev, u32 attr, int channel,
			     long *val)
{
	struct adt7475_data *data;
	int nr = adt7475_temp_nr(attr);

	if (nr < 0)
		return nr;

	data = adt7475_update_device(dev);
	if (IS_ERR(data))
		return PTR_ERR(data);

	*val = adt7475_temp_value(data, nr, channel);

	return 0;
}

static int adt7475_temp_write(struct device *dev, u32 attr, int channel,
			      long val)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	int nr = adt7475_temp_nr(attr);

	switch (nr) {
	case MIN:
	case MAX:
	case OFFSET:
	case THERM:
	case HYSTERSIS:
		adt7475_temp_set(data, nr, channel, val);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_fan_read(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct adt7475_data *data = adt7475_update_device(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_fan_input:
		*val = tach2rpm(data->tach[INPUT][channel]);
		return 0;
	case hwmon_fan_min:
		*val = tach2rpm(data->tach[MIN][channel]);
		return 0;
	case hwmon_fan_alarm:
		*val = (data->alarms >> (channel + 10)) & 1;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_fan_write(struct device *dev, u32 attr, int channel,
			     long val)
{
	struct adt7475_data *data = dev_get_drvdata(dev);

	if (attr != hwmon_fan_min)
		return -EOPNOTSUPP;

	/* fanN_min takes an unsigned value */
	if (val < 0)
		return -EINVAL;

	mutex_lock(&data->lock);

	data->tach[MIN][channel] = rpm2tach(val);

	adt7475_write_word(data->client, TACH_MIN_REG(channel),
			   data->tach[MIN][channel]);

	mutex_unlock(&data->lock);

	return 0;
}

static int adt7475_pwm_read(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct adt7475_data *data = adt7475_update_device(dev);
	int idx;

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_pwm_input:
		*val = data->pwm[INPUT][channel];
		return 0;
	case hwmon_pwm_enable:
		*val = data->pwmctl[channel];
		return 0;
	case hwmon_pwm_auto_channels_temp:
		*val = data->pwmchan[channel];
		return 0;
	case hwmon_pwm_freq:
		idx = clamp_val(data->range[channel] & 0xf, 0,
				ARRAY_SIZE(pwmfreq_table) - 1);
		*val = pwmfreq_table[idx];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_pwm_write(struct device *dev, u32 attr, int channel,
			     long val)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret = 0;
	int out;

	switch (attr) {
	case hwmon_pwm_input:
		ret = adt7475_pwm_set(data, INPUT, channel, val);
		break;

	case hwmon_pwm_enable:
		mutex_lock(&data->lock);
		ret = adt7475_pwm_check_held(data, channel);
		if (!ret) {
			/* Read Modify Write PWM values */
			adt7475_read_pwm(client, channel);
			ret = hw_set_pwm(client, channel, val,
					 data->pwmchan[channel]);
		}
		mutex_unlock(&data->lock);
		break;

	case hwmon_pwm_auto_channels_temp:
		mutex_lock(&data->lock);
		ret = adt7475_pwm_check_held(data, channel);
		if (!ret) {
			/* Read Modify Write PWM values */
			adt7475_read_pwm(client, channel);
			ret = hw_set_pwm(client, channel,
					 data->pwmctl[channel], val);
		}
		mutex_unlock(&data->lock);
		break;

	case hwmon_pwm_freq:
		out = find_closest(val, pwmfreq_table,
				   ARRAY_SIZE(pwmfreq_table));

		mutex_lock(&data->lock);

		ret = adt7475_pwm_check_held(data, channel);
		if (ret) {
			mutex_unlock(&data->lock);
			break;
		}

		data->range[channel] =
			adt7475_read(TEMP_TRANGE_REG(channel));
		data->range[channel] &= ~0xf;
		data->range[channel] |= out;

		i2c_smbus_write_byte_data(client, TEMP_TRANGE_REG(channel),
					  data->range[channel]);

		mutex_unlock(&data->lock);
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int adt7475_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type, u32 attr,
			      int channel, long *val)
{
	switch (type) {
	case hwmon_in:
		return adt7475_in_read(dev, attr, channel, val);
	case hwmon_temp:
		return adt7475_temp_read(dev, attr, channel, val);
	case hwmon_fan:
		return adt7475_fan_read(dev, attr, channel, val);
	case hwmon_pwm:
		return adt7475_pwm_read(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int adt7475_hwmon_write(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long val)
{
	switch (type) {
	case hwmon_in:
		return adt7475_in_write(dev, attr, channel, val);
	case hwmon_temp:
		return adt7475_temp_write(dev, attr, channel, val);
	case hwmon_fan:
		return adt7475_fan_write(dev, attr, channel, val);
	case hwmon_pwm:
		return adt7475_pwm_write(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t adt7475_is_visible(const void *_data,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	const struct adt7475_data *data = _data;

	switch (type) {
	case hwmon_in:
		if (!(data->has_voltage & BIT(channel)))
			return 0;
		if (attr == hwmon_in_min || attr == hwmon_in_max)
			return 0644;
		return 0444;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_alarm:
		case hwmon_temp_fault:
			return 0444;
		default:
			return 0644;
		}
	case hwmon_fan:
		if (channel == 3 && !data->has_fan4)
			return 0;
		if (attr == hwmon_fan_min)
			return 0644;
		return 0444;
	case hwmon_pwm:
		if (channel == 1 && !data->has_pwm2)
			return 0;
		return 0644;
	default:
		return 0;
	}
}

static const struct hwmon_ops adt7475_hwmon_ops = {
	.is_visible = adt7475_is_visible,
	.read = adt7475_hwmon_read,
	.write = adt7475_hwmon_write,
};

#define ADT7475_IN_CONFIG	(HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | \
				 HWMON_I_ALARM)
#define ADT7475_TEMP_CONFIG	(HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX | \
				 HWMON_T_OFFSET | HWMON_T_CRIT | \
				 HWMON_T_CRIT_HYST | HWMON_T_ALARM)
#define ADT7475_FAN_CONFIG	(HWMON_F_INPUT | HWMON_F_MIN | HWMON_F_ALARM)
#define ADT7475_PWM_CONFIG	(HWMON_PWM_INPUT | HWMON_PWM_ENABLE | \
				 HWMON_PWM_FREQ | HWMON_PWM_AUTO_CHANNELS_TEMP)

static const struct hwmon_channel_info * const adt7475_info[] = {
	/* The hwmon core only looks for HWMON_C_REGISTER_TZ in info[0] */
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(in,
			   ADT7475_IN_CONFIG,	/* in0: +2.5V */
			   ADT7475_IN_CONFIG,	/* in1: Vccp */
			   ADT7475_IN_CONFIG,	/* in2: Vcc */
			   ADT7475_IN_CONFIG,	/* in3: +5V */
			   ADT7475_IN_CONFIG,	/* in4: +12V */
			   ADT7475_IN_CONFIG,	/* in5: Vtt */
			   ADT7475_IN_CONFIG),	/* in6: Imon */
	HWMON_CHANNEL_INFO(temp,
			   ADT7475_TEMP_CONFIG | HWMON_T_FAULT,	/* remote 1 */
			   ADT7475_TEMP_CONFIG,			/* local */
			   ADT7475_TEMP_CONFIG | HWMON_T_FAULT),	/* remote 2 */
	HWMON_CHANNEL_INFO(fan,
			   ADT7475_FAN_CONFIG,
			   ADT7475_FAN_CONFIG,
			   ADT7475_FAN_CONFIG,
			   ADT7475_FAN_CONFIG),
	HWMON_CHANNEL_INFO(pwm,
			   ADT7475_PWM_CONFIG,
			   ADT7475_PWM_CONFIG,
			   ADT7475_PWM_CONFIG),
	NULL
};

static const struct hwmon_chip_info adt7475_chip_info = {
	.ops = &adt7475_hwmon_ops,
	.info = adt7475_info,
};

static SENSOR_DEVICE_ATTR_2_RW(temp1_auto_point1_temp, temp, AUTOMIN, 0);
static SENSOR_DEVICE_ATTR_2_RW(temp1_auto_point2_temp, point2, 0, 0);
static SENSOR_DEVICE_ATTR_2_RW(temp1_smoothing, temp_st, 0, 0);
static SENSOR_DEVICE_ATTR_2_RW(temp2_auto_point1_temp, temp, AUTOMIN, 1);
static SENSOR_DEVICE_ATTR_2_RW(temp2_auto_point2_temp, point2, 0, 1);
static SENSOR_DEVICE_ATTR_2_RW(temp2_smoothing, temp_st, 0, 1);
static SENSOR_DEVICE_ATTR_2_RW(temp3_auto_point1_temp, temp, AUTOMIN, 2);
static SENSOR_DEVICE_ATTR_2_RW(temp3_auto_point2_temp, point2, 0, 2);
static SENSOR_DEVICE_ATTR_2_RW(temp3_smoothing, temp_st, 0, 2);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point1_pwm, pwm, MIN, 0);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point2_pwm, pwm, MAX, 0);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_stall_disable, stall_disable, 0, 0);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point1_pwm, pwm, MIN, 1);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point2_pwm, pwm, MAX, 1);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_stall_disable, stall_disable, 0, 1);
static SENSOR_DEVICE_ATTR_2_RW(pwm3_auto_point1_pwm, pwm, MIN, 2);
static SENSOR_DEVICE_ATTR_2_RW(pwm3_auto_point2_pwm, pwm, MAX, 2);
static SENSOR_DEVICE_ATTR_2_RW(pwm3_stall_disable, stall_disable, 0, 2);

/* Non-standard name, might need revisiting */
static DEVICE_ATTR_RW(pwm_use_point2_pwm_at_crit);

static DEVICE_ATTR_RW(vrm);
static DEVICE_ATTR_RO(cpu0_vid);

static struct attribute *adt7475_attrs[] = {
	&sensor_dev_attr_temp1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_temp1_smoothing.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_temp2_smoothing.dev_attr.attr,
	&sensor_dev_attr_temp3_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp3_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_temp3_smoothing.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_stall_disable.dev_attr.attr,
	&sensor_dev_attr_pwm3_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm3_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm3_stall_disable.dev_attr.attr,
	&dev_attr_pwm_use_point2_pwm_at_crit.attr,
	NULL,
};

static struct attribute *pwm2_attrs[] = {
	&sensor_dev_attr_pwm2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_stall_disable.dev_attr.attr,
	NULL
};

static struct attribute *vid_attrs[] = {
	&dev_attr_cpu0_vid.attr,
	&dev_attr_vrm.attr,
	NULL
};

static const struct attribute_group adt7475_attr_group = { .attrs = adt7475_attrs };
static const struct attribute_group pwm2_attr_group = { .attrs = pwm2_attrs };
static const struct attribute_group vid_attr_group = { .attrs = vid_attrs };

static int adt7475_detect(struct i2c_client *client,
			  struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int vendid, devid, devid2;
	const char *name;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	vendid = adt7475_read(REG_VENDID);
	devid2 = adt7475_read(REG_DEVID2);
	if (vendid != 0x41 ||		/* Analog Devices */
	    (devid2 & 0xf8) != 0x68)
		return -ENODEV;

	devid = adt7475_read(REG_DEVID);
	if (devid == 0x73)
		name = "adt7473";
	else if (devid == 0x75 && client->addr == 0x2e)
		name = "adt7475";
	else if (devid == 0x76)
		name = "adt7476";
	else if ((devid2 & 0xfc) == 0x6c)
		name = "adt7490";
	else {
		dev_dbg(&adapter->dev,
			"Couldn't detect an ADT7473/75/76/90 part at "
			"0x%02x\n", (unsigned int)client->addr);
		return -ENODEV;
	}

	strscpy(info->type, name, I2C_NAME_SIZE);

	return 0;
}

static int adt7475_update_limits(struct i2c_client *client)
{
	struct adt7475_data *data = i2c_get_clientdata(client);
	int i;
	int ret;

	ret = adt7475_read(REG_CONFIG4);
	if (ret < 0)
		return ret;
	data->config4 = ret;

	ret = adt7475_read(REG_CONFIG5);
	if (ret < 0)
		return ret;
	data->config5 = ret;

	for (i = 0; i < ADT7475_VOLTAGE_COUNT; i++) {
		if (!(data->has_voltage & (1 << i)))
			continue;
		/* Adjust values so they match the input precision */
		ret = adt7475_read(VOLTAGE_MIN_REG(i));
		if (ret < 0)
			return ret;
		data->voltage[MIN][i] = ret << 2;

		ret = adt7475_read(VOLTAGE_MAX_REG(i));
		if (ret < 0)
			return ret;
		data->voltage[MAX][i] = ret << 2;
	}

	if (data->has_voltage & (1 << 5)) {
		ret = adt7475_read(REG_VTT_MIN);
		if (ret < 0)
			return ret;
		data->voltage[MIN][5] = ret << 2;

		ret = adt7475_read(REG_VTT_MAX);
		if (ret < 0)
			return ret;
		data->voltage[MAX][5] = ret << 2;
	}

	if (data->has_voltage & (1 << 6)) {
		ret = adt7475_read(REG_IMON_MIN);
		if (ret < 0)
			return ret;
		data->voltage[MIN][6] = ret << 2;

		ret = adt7475_read(REG_IMON_MAX);
		if (ret < 0)
			return ret;
		data->voltage[MAX][6] = ret << 2;
	}

	for (i = 0; i < ADT7475_TEMP_COUNT; i++) {
		/* Adjust values so they match the input precision */
		ret = adt7475_read(TEMP_MIN_REG(i));
		if (ret < 0)
			return ret;
		data->temp[MIN][i] = ret << 2;

		ret = adt7475_read(TEMP_MAX_REG(i));
		if (ret < 0)
			return ret;
		data->temp[MAX][i] = ret << 2;

		ret = adt7475_read(TEMP_TMIN_REG(i));
		if (ret < 0)
			return ret;
		data->temp[AUTOMIN][i] = ret << 2;

		ret = adt7475_read(TEMP_THERM_REG(i));
		if (ret < 0)
			return ret;
		data->temp[THERM][i] = ret << 2;

		ret = adt7475_read(TEMP_OFFSET_REG(i));
		if (ret < 0)
			return ret;
		data->temp[OFFSET][i] = ret;
	}
	adt7475_read_hystersis(client);

	for (i = 0; i < ADT7475_TACH_COUNT; i++) {
		if (i == 3 && !data->has_fan4)
			continue;
		ret = adt7475_read_word(client, TACH_MIN_REG(i));
		if (ret < 0)
			return ret;
		data->tach[MIN][i] = ret;
	}

	for (i = 0; i < ADT7475_PWM_COUNT; i++) {
		if (i == 1 && !data->has_pwm2)
			continue;
		ret = adt7475_read(PWM_MAX_REG(i));
		if (ret < 0)
			return ret;
		data->pwm[MAX][i] = ret;

		ret = adt7475_read(PWM_MIN_REG(i));
		if (ret < 0)
			return ret;
		data->pwm[MIN][i] = ret;
		/* Set the channel and control information */
		adt7475_read_pwm(client, i);
	}

	ret = adt7475_read(TEMP_TRANGE_REG(0));
	if (ret < 0)
		return ret;
	data->range[0] = ret;

	ret = adt7475_read(TEMP_TRANGE_REG(1));
	if (ret < 0)
		return ret;
	data->range[1] = ret;

	ret = adt7475_read(TEMP_TRANGE_REG(2));
	if (ret < 0)
		return ret;
	data->range[2] = ret;

	return 0;
}

static int load_config3(const struct i2c_client *client, const char *propname)
{
	const char *function;
	u8 config3;
	int ret;

	ret = device_property_read_string(&client->dev, propname, &function);
	if (!ret) {
		ret = adt7475_read(REG_CONFIG3);
		if (ret < 0)
			return ret;

		config3 = ret & ~CONFIG3_SMBALERT;
		if (!strcmp("pwm2", function))
			;
		else if (!strcmp("smbalert#", function))
			config3 |= CONFIG3_SMBALERT;
		else
			return -EINVAL;

		return i2c_smbus_write_byte_data(client, REG_CONFIG3, config3);
	}

	return 0;
}

static int load_config4(const struct i2c_client *client, const char *propname)
{
	const char *function;
	u8 config4;
	int ret;

	ret = device_property_read_string(&client->dev, propname, &function);
	if (!ret) {
		ret = adt7475_read(REG_CONFIG4);
		if (ret < 0)
			return ret;

		config4 = ret & ~CONFIG4_PINFUNC;

		if (!strcmp("tach4", function))
			;
		else if (!strcmp("therm#", function))
			config4 |= CONFIG4_THERM;
		else if (!strcmp("smbalert#", function))
			config4 |= CONFIG4_SMBALERT;
		else if (!strcmp("gpio", function))
			config4 |= CONFIG4_PINFUNC;
		else
			return -EINVAL;

		return i2c_smbus_write_byte_data(client, REG_CONFIG4, config4);
	}

	return 0;
}

static int load_config(const struct i2c_client *client, enum chips chip)
{
	int err;
	const char *prop1, *prop2;

	switch (chip) {
	case adt7473:
	case adt7475:
		prop1 = "adi,pin5-function";
		prop2 = "adi,pin9-function";
		break;
	case adt7476:
	case adt7490:
		prop1 = "adi,pin10-function";
		prop2 = "adi,pin14-function";
		break;
	}

	err = load_config3(client, prop1);
	if (err) {
		dev_err(&client->dev, "failed to configure %s\n", prop1);
		return err;
	}

	err = load_config4(client, prop2);
	if (err) {
		dev_err(&client->dev, "failed to configure %s\n", prop2);
		return err;
	}

	return 0;
}

static int set_property_bit(const struct i2c_client *client, char *property,
			    u8 *config, u8 bit_index)
{
	u32 prop_value = 0;
	int ret = device_property_read_u32(&client->dev, property,
					   &prop_value);

	if (!ret) {
		if (prop_value)
			*config |= (1 << bit_index);
		else
			*config &= ~(1 << bit_index);
	}

	return ret;
}

static int load_attenuators(const struct i2c_client *client, enum chips chip,
			    struct adt7475_data *data)
{
	switch (chip) {
	case adt7476:
	case adt7490:
		set_property_bit(client, "adi,bypass-attenuator-in0",
				 &data->config4, 4);
		set_property_bit(client, "adi,bypass-attenuator-in1",
				 &data->config4, 5);
		set_property_bit(client, "adi,bypass-attenuator-in3",
				 &data->config4, 6);
		set_property_bit(client, "adi,bypass-attenuator-in4",
				 &data->config4, 7);

		return i2c_smbus_write_byte_data(client, REG_CONFIG4,
						 data->config4);
	case adt7473:
	case adt7475:
		set_property_bit(client, "adi,bypass-attenuator-in1",
				 &data->config2, 5);

		return i2c_smbus_write_byte_data(client, REG_CONFIG2,
						 data->config2);
	}

	return 0;
}

static int adt7475_set_pwm_polarity(struct i2c_client *client)
{
	u32 states[ADT7475_PWM_COUNT];
	int ret, i;
	u8 val;

	ret = device_property_read_u32_array(&client->dev,
					     "adi,pwm-active-state", states,
					     ARRAY_SIZE(states));
	if (ret)
		return ret;

	for (i = 0; i < ADT7475_PWM_COUNT; i++) {
		ret = adt7475_read(PWM_CONFIG_REG(i));
		if (ret < 0)
			return ret;
		val = ret;
		if (states[i])
			val &= ~BIT(4);
		else
			val |= BIT(4);

		ret = i2c_smbus_write_byte_data(client, PWM_CONFIG_REG(i), val);
		if (ret)
			return ret;
	}

	return 0;
}

struct adt7475_pwm_config {
	int index;
	int freq;
	int flags;
	int duty;
};

static int _adt7475_pwm_properties_parse_args(u32 args[4], struct adt7475_pwm_config *cfg)
{
	int freq_hz;
	int duty;

	if (args[1] == 0)
		return -EINVAL;

	freq_hz = 1000000000UL / args[1];
	if (args[3] >= args[1])
		duty = 255;
	else
		duty = div_u64(255ULL * args[3], args[1]);

	cfg->index = args[0];
	cfg->freq = find_closest(freq_hz, pwmfreq_table, ARRAY_SIZE(pwmfreq_table));
	cfg->flags = args[2];
	cfg->duty = duty;

	return 0;
}

static int adt7475_pwm_properties_parse_reference_args(struct fwnode_handle *fwnode,
						       struct adt7475_pwm_config *cfg)
{
	int ret, i;
	struct fwnode_reference_args rargs = {};
	u32 args[4] = {};

	ret = fwnode_property_get_reference_args(fwnode, "pwms", "#pwm-cells", 0, 0, &rargs);
	if (ret)
		return ret;

	if (rargs.nargs != 3 && rargs.nargs != 4) {
		fwnode_handle_put(rargs.fwnode);
		return -EINVAL;
	}

	/* Let duty_cycle default to period */
	args[3] = rargs.args[1];

	for (i = 0; i < rargs.nargs; i++)
		args[i] = rargs.args[i];

	ret = _adt7475_pwm_properties_parse_args(args, cfg);

	fwnode_handle_put(rargs.fwnode);

	return ret;
}

static int adt7475_pwm_properties_parse_args(struct fwnode_handle *fwnode,
					     struct adt7475_pwm_config *cfg)
{
	int ret;
	u32 args[4] = {};
	size_t n_vals = fwnode_property_count_u32(fwnode, "pwms");

	if (n_vals != 3 && n_vals != 4)
		return -EOVERFLOW;

	ret = fwnode_property_read_u32_array(fwnode, "pwms", args, n_vals);
	if (ret)
		return ret;

	/*
	 * If there are no item to define the duty_cycle, default it to the
	 * period.
	 */
	if (n_vals == 3)
		args[3] = args[1];

	return _adt7475_pwm_properties_parse_args(args, cfg);
}

static int adt7475_fan_pwm_config(struct i2c_client *client)
{
	struct adt7475_data *data = i2c_get_clientdata(client);
	struct adt7475_pwm_config cfg = {};
	int ret;

	device_for_each_child_node_scoped(&client->dev, child) {
		if (!fwnode_property_present(child, "pwms"))
			continue;

		if (is_of_node(child))
			ret = adt7475_pwm_properties_parse_reference_args(child, &cfg);
		else
			ret = adt7475_pwm_properties_parse_args(child, &cfg);

		if (cfg.index >= ADT7475_PWM_COUNT)
			return -EINVAL;

		ret = adt7475_read(PWM_CONFIG_REG(cfg.index));
		if (ret < 0)
			return ret;
		data->pwm[CONTROL][cfg.index] = ret;
		if (cfg.flags & PWM_POLARITY_INVERTED)
			data->pwm[CONTROL][cfg.index] |= BIT(4);
		else
			data->pwm[CONTROL][cfg.index] &= ~BIT(4);

		/* Force to manual mode so PWM values take effect */
		data->pwm[CONTROL][cfg.index] &= ~0xE0;
		data->pwm[CONTROL][cfg.index] |= 0x07 << 5;

		ret = i2c_smbus_write_byte_data(client, PWM_CONFIG_REG(cfg.index),
						data->pwm[CONTROL][cfg.index]);
		if (ret)
			return ret;

		data->pwm[INPUT][cfg.index] = cfg.duty;
		ret = i2c_smbus_write_byte_data(client, PWM_REG(cfg.index),
						data->pwm[INPUT][cfg.index]);
		if (ret)
			return ret;

		data->range[cfg.index] = adt7475_read(TEMP_TRANGE_REG(cfg.index));
		data->range[cfg.index] &= ~0xf;
		data->range[cfg.index] |= cfg.freq;

		ret = i2c_smbus_write_byte_data(client, TEMP_TRANGE_REG(cfg.index),
						data->range[cfg.index]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * PWM framework provider. Requesting a channel only marks it held: the
 * hardware is left alone until the consumer applies a state.
 *
 * Limitations:
 * - Only the fixed periods of pwmfreq_table (11 Hz to 22.5 kHz) are
 *   supported. The duty cycle is set in 1/255 steps of the period.
 * - The duty offset is not supported and is always 0.
 * - Disabling a channel in manual mode sets its duty cycle to 0, so the
 *   output is constant low. A channel in any other mode is left alone.
 * - Mode, duty cycle and frequency are separate register writes, in that
 *   order. An update is not atomic, and the output can briefly run with
 *   a mix of old and new settings.
 */
static int adt7475_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct adt7475_data *data = pwmchip_get_drvdata(chip);

	/* The PWM2 pin is the SMBALERT# output */
	if (pwm->hwpwm == 1 && !data->has_pwm2)
		return -ENODEV;

	mutex_lock(&data->lock);
	data->pwm_held |= BIT(pwm->hwpwm);
	mutex_unlock(&data->lock);

	return 0;
}

static void adt7475_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct adt7475_data *data = pwmchip_get_drvdata(chip);

	mutex_lock(&data->lock);
	data->pwm_held &= ~BIT(pwm->hwpwm);
	mutex_unlock(&data->lock);
}

/*
 * The waveform API has no polarity: duty is the high time in 1/255 steps of
 * the period.
 */
struct adt7475_pwm_wfhw {
	bool enabled;
	u8 duty;
	u8 freq_idx;	/* Index into pwmfreq_table */
};

static u64 adt7475_pwm_period(int freq_idx)
{
	return NSEC_PER_SEC / pwmfreq_table[freq_idx];
}

static int adt7475_pwm_round_waveform_tohw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const struct pwm_waveform *wf,
					   void *_wfhw)
{
	struct adt7475_pwm_wfhw *wfhw = _wfhw;
	u64 period;
	int i;

	if (!wf->period_length_ns) {
		*wfhw = (struct adt7475_pwm_wfhw){ .enabled = false };
		return 0;
	}

	/* The longest supported period that does not exceed the request */
	for (i = 0; i < ARRAY_SIZE(pwmfreq_table) - 1; i++)
		if (adt7475_pwm_period(i) <= wf->period_length_ns)
			break;
	period = adt7475_pwm_period(i);

	wfhw->enabled = true;
	wfhw->freq_idx = i;
	if (wf->duty_length_ns >= period)
		wfhw->duty = 0xFF;
	else
		wfhw->duty = mul_u64_u64_div_u64(wf->duty_length_ns, 0xFF,
						 period);

	/* Requested period shorter than the shortest supported one */
	return period > wf->period_length_ns ? 1 : 0;
}

static int adt7475_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					     struct pwm_device *pwm,
					     const void *_wfhw,
					     struct pwm_waveform *wf)
{
	const struct adt7475_pwm_wfhw *wfhw = _wfhw;
	u64 period;

	if (!wfhw->enabled) {
		*wf = (struct pwm_waveform){ };
		return 0;
	}

	period = adt7475_pwm_period(wfhw->freq_idx);
	wf->period_length_ns = period;
	wf->duty_length_ns = DIV_ROUND_UP_ULL((u64)wfhw->duty * period, 0xFF);
	wf->duty_offset_ns = 0;

	return 0;
}

/*
 * A channel that is not in manual mode is not driven by the PWM framework
 * and reads as disabled. A consumer's first enabled request then always
 * differs from the read-back state, so the core does not skip it.
 */
static int adt7475_pwm_read_waveform(struct pwm_chip *chip,
				     struct pwm_device *pwm, void *_wfhw)
{
	struct adt7475_data *data = pwmchip_get_drvdata(chip);
	struct i2c_client *client = data->client;
	struct adt7475_pwm_wfhw *wfhw = _wfhw;
	int config, duty, range;

	mutex_lock(&data->lock);
	config = adt7475_read(PWM_CONFIG_REG(pwm->hwpwm));
	duty = adt7475_read(PWM_REG(pwm->hwpwm));
	range = adt7475_read(TEMP_TRANGE_REG(pwm->hwpwm));
	mutex_unlock(&data->lock);

	if (config < 0)
		return config;
	if (duty < 0)
		return duty;
	if (range < 0)
		return range;

	if (((config >> 5) & 7) != 7) {
		*wfhw = (struct adt7475_pwm_wfhw){ .enabled = false };
		return 0;
	}

	wfhw->enabled = true;
	wfhw->duty = config & BIT(4) ? 0xFF - duty : duty;
	wfhw->freq_idx = clamp_val(range & 0xf, 0,
				   ARRAY_SIZE(pwmfreq_table) - 1);

	return 0;
}

/*
 * Switch the channel to manual mode with normal polarity and write the duty
 * cycle. A negative freq_idx keeps the current frequency. A negative duty
 * means disabled: a channel in manual mode gets duty 0, a channel in another
 * mode is left alone. Each cache is updated as soon as its register write
 * succeeds. Call with data->lock held.
 */
static int adt7475_pwm_write_hw(struct adt7475_data *data, int index,
				int duty, int freq_idx)
{
	struct i2c_client *client = data->client;
	int ret;
	u8 val;

	ret = adt7475_read(PWM_CONFIG_REG(index));
	if (ret < 0)
		return ret;
	if (duty < 0) {
		if (((ret >> 5) & 7) != 7)
			return 0;
		duty = 0;
	}
	val = (ret & ~(0xE0 | BIT(4))) | (7 << 5);
	if (val != ret) {
		ret = i2c_smbus_write_byte_data(client, PWM_CONFIG_REG(index),
						val);
		if (ret)
			return ret;
	}
	data->pwm[CONTROL][index] = val;
	data->pwmctl[index] = 1;

	ret = i2c_smbus_write_byte_data(client, PWM_REG(index), duty);
	if (ret)
		return ret;
	data->pwm[INPUT][index] = duty;

	if (freq_idx < 0)
		return 0;

	ret = adt7475_read(TEMP_TRANGE_REG(index));
	if (ret < 0)
		return ret;
	val = (ret & ~0xf) | freq_idx;
	if (val != ret) {
		ret = i2c_smbus_write_byte_data(client, TEMP_TRANGE_REG(index),
						val);
		if (ret)
			return ret;
	}
	data->range[index] = val;

	return 0;
}

static int adt7475_pwm_write_waveform(struct pwm_chip *chip,
				      struct pwm_device *pwm,
				      const void *_wfhw)
{
	struct adt7475_data *data = pwmchip_get_drvdata(chip);
	const struct adt7475_pwm_wfhw *wfhw = _wfhw;
	int ret;

	mutex_lock(&data->lock);
	if (wfhw->enabled)
		ret = adt7475_pwm_write_hw(data, pwm->hwpwm, wfhw->duty,
					   wfhw->freq_idx);
	else
		ret = adt7475_pwm_write_hw(data, pwm->hwpwm, -1, -1);
	mutex_unlock(&data->lock);

	return ret;
}

static const struct pwm_ops adt7475_pwm_ops = {
	.request = adt7475_pwm_request,
	.free = adt7475_pwm_free,
	.sizeof_wfhw = sizeof(struct adt7475_pwm_wfhw),
	.round_waveform_tohw = adt7475_pwm_round_waveform_tohw,
	.round_waveform_fromhw = adt7475_pwm_round_waveform_fromhw,
	.read_waveform = adt7475_pwm_read_waveform,
	.write_waveform = adt7475_pwm_write_waveform,
};

static int adt7475_pwm_register(struct i2c_client *client)
{
	struct adt7475_data *data = i2c_get_clientdata(client);
	struct pwm_chip *chip;

	chip = devm_pwmchip_alloc(&client->dev, ADT7475_PWM_COUNT, 0);
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	chip->ops = &adt7475_pwm_ops;
	pwmchip_set_drvdata(chip, data);

	return devm_pwmchip_add(&client->dev, chip);
}

static int adt7475_probe(struct i2c_client *client)
{
	enum chips chip;
	static const char * const names[] = {
		[adt7473] = "ADT7473",
		[adt7475] = "ADT7475",
		[adt7476] = "ADT7476",
		[adt7490] = "ADT7490",
	};

	struct adt7475_data *data;
	struct device *hwmon_dev;
	int i, ret = 0, revision, group_num = 0;
	u8 config3;

	data = devm_kzalloc_obj(&client->dev, *data);
	if (data == NULL)
		return -ENOMEM;

	mutex_init(&data->lock);
	data->client = client;
	i2c_set_clientdata(client, data);

	chip = (uintptr_t)i2c_get_match_data(client);

	/* Initialize device-specific values */
	switch (chip) {
	case adt7476:
		data->has_voltage = 0x0e;	/* in1 to in3 */
		revision = adt7475_read(REG_DEVID2) & 0x07;
		break;
	case adt7490:
		data->has_voltage = 0x7e;	/* in1 to in6 */
		revision = adt7475_read(REG_DEVID2) & 0x03;
		if (revision == 0x03)
			revision += adt7475_read(REG_DEVREV2);
		break;
	default:
		data->has_voltage = 0x06;	/* in1, in2 */
		revision = adt7475_read(REG_DEVID2) & 0x07;
	}

	ret = load_config(client, chip);
	if (ret)
		return ret;

	config3 = adt7475_read(REG_CONFIG3);
	/* Pin PWM2 may alternatively be used for ALERT output */
	if (!(config3 & CONFIG3_SMBALERT))
		data->has_pwm2 = 1;
	/* Meaning of this bit is inverted for the ADT7473-1 */
	if (chip == adt7473 && revision >= 1)
		data->has_pwm2 = !data->has_pwm2;

	data->config4 = adt7475_read(REG_CONFIG4);
	/* Pin TACH4 may alternatively be used for THERM */
	if ((data->config4 & CONFIG4_PINFUNC) == 0x0)
		data->has_fan4 = 1;

	/*
	 * THERM configuration is more complex on the ADT7476 and ADT7490,
	 * because 2 different pins (TACH4 and +2.5 Vin) can be used for
	 * this function
	 */
	if (chip == adt7490) {
		if ((data->config4 & CONFIG4_PINFUNC) == 0x1 &&
		    !(config3 & CONFIG3_THERM))
			data->has_fan4 = 1;
	}
	if (chip == adt7476 || chip == adt7490) {
		if (!(config3 & CONFIG3_THERM) ||
		    (data->config4 & CONFIG4_PINFUNC) == 0x1)
			data->has_voltage |= (1 << 0);		/* in0 */
	}

	/*
	 * On the ADT7476, the +12V input pin may instead be used as VID5,
	 * and VID pins may alternatively be used as GPIO
	 */
	if (chip == adt7476) {
		u8 vid = adt7475_read(REG_VID);
		if (!(vid & VID_VIDSEL))
			data->has_voltage |= (1 << 4);		/* in4 */

		data->has_vid = !(adt7475_read(REG_CONFIG5) & CONFIG5_VIDGPIO);
	}

	/* Voltage attenuators can be bypassed, globally or individually */
	data->config2 = adt7475_read(REG_CONFIG2);
	ret = load_attenuators(client, chip, data);
	if (ret)
		dev_warn(&client->dev, "Error configuring attenuator bypass\n");

	if (data->config2 & CONFIG2_ATTN) {
		data->bypass_attn = (0x3 << 3) | 0x3;
	} else {
		data->bypass_attn = ((data->config4 & CONFIG4_ATTN_IN10) >> 4) |
				    ((data->config4 & CONFIG4_ATTN_IN43) >> 3);
	}
	data->bypass_attn &= data->has_voltage;

	/*
	 * Call adt7475_read_pwm for all pwm's as this will reprogram any
	 * pwm's which are disabled to manual mode with 0% duty cycle
	 */
	for (i = 0; i < ADT7475_PWM_COUNT; i++)
		adt7475_read_pwm(client, i);

	ret = adt7475_set_pwm_polarity(client);
	if (ret && ret != -EINVAL)
		dev_warn(&client->dev, "Error configuring pwm polarity\n");

	ret = adt7475_fan_pwm_config(client);
	if (ret)
		dev_warn(&client->dev, "Error %d configuring fan/pwm\n", ret);

	/* Start monitoring */
	switch (chip) {
	case adt7475:
	case adt7476:
		i2c_smbus_write_byte_data(client, REG_CONFIG1,
					  adt7475_read(REG_CONFIG1) | 0x01);
		break;
	default:
		break;
	}

	/*
	 * Read the limits and settings once, before registering: the hwmon
	 * core reads the thermal sensors while registering, and that needs
	 * the temperature encoding from CONFIG5.
	 */
	ret = adt7475_update_limits(client);
	if (ret)
		return ret;

	data->groups[group_num++] = &adt7475_attr_group;

	/* Features that can be disabled individually */
	if (data->has_pwm2) {
		data->groups[group_num++] = &pwm2_attr_group;
	}
	if (data->has_vid) {
		data->vrm = vid_which_vrm();
		data->groups[group_num] = &vid_attr_group;
	}

	/* register device with all the acquired attributes */
	hwmon_dev = devm_hwmon_device_register_with_info(&client->dev,
							 client->name, data,
							 &adt7475_chip_info,
							 data->groups);

	if (IS_ERR(hwmon_dev)) {
		ret = PTR_ERR(hwmon_dev);
		return ret;
	}

	if (IS_ENABLED(CONFIG_PWM) &&
	    device_property_present(&client->dev, "#pwm-cells")) {
		ret = adt7475_pwm_register(client);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "Failed to register PWM chip\n");
	}

	dev_info(&client->dev, "%s device, revision %d\n",
		 names[chip], revision);
	if ((data->has_voltage & 0x11) || data->has_fan4 || data->has_pwm2)
		dev_info(&client->dev, "Optional features:%s%s%s%s%s\n",
			 (data->has_voltage & (1 << 0)) ? " in0" : "",
			 (data->has_voltage & (1 << 4)) ? " in4" : "",
			 data->has_fan4 ? " fan4" : "",
			 data->has_pwm2 ? " pwm2" : "",
			 data->has_vid ? " vid" : "");
	if (data->bypass_attn)
		dev_info(&client->dev, "Bypassing attenuators on:%s%s%s%s\n",
			 (data->bypass_attn & (1 << 0)) ? " in0" : "",
			 (data->bypass_attn & (1 << 1)) ? " in1" : "",
			 (data->bypass_attn & (1 << 3)) ? " in3" : "",
			 (data->bypass_attn & (1 << 4)) ? " in4" : "");

	return 0;
}

static struct i2c_driver adt7475_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "adt7475",
		.of_match_table = adt7475_of_match,
	},
	.probe		= adt7475_probe,
	.id_table	= adt7475_id,
	.detect		= adt7475_detect,
	.address_list	= normal_i2c,
};

static void adt7475_read_hystersis(struct i2c_client *client)
{
	struct adt7475_data *data = i2c_get_clientdata(client);

	data->temp[HYSTERSIS][0] = (u16) adt7475_read(REG_REMOTE1_HYSTERSIS);
	data->temp[HYSTERSIS][1] = data->temp[HYSTERSIS][0];
	data->temp[HYSTERSIS][2] = (u16) adt7475_read(REG_REMOTE2_HYSTERSIS);
}

static void adt7475_read_pwm(struct i2c_client *client, int index)
{
	struct adt7475_data *data = i2c_get_clientdata(client);
	unsigned int v;

	data->pwm[CONTROL][index] = adt7475_read(PWM_CONFIG_REG(index));

	/*
	 * Figure out the internal value for pwmctrl and pwmchan
	 * based on the current settings
	 */
	v = (data->pwm[CONTROL][index] >> 5) & 7;

	if (v == 3)
		data->pwmctl[index] = 0;
	else if (v == 7)
		data->pwmctl[index] = 1;
	else if (v == 4) {
		/*
		 * The fan is disabled - we don't want to
		 * support that, so change to manual mode and
		 * set the duty cycle to 0 instead
		 */
		data->pwm[INPUT][index] = 0;
		data->pwm[CONTROL][index] &= ~0xE0;
		data->pwm[CONTROL][index] |= (7 << 5);

		i2c_smbus_write_byte_data(client, PWM_REG(index),
					  data->pwm[INPUT][index]);

		i2c_smbus_write_byte_data(client, PWM_CONFIG_REG(index),
					  data->pwm[CONTROL][index]);

		data->pwmctl[index] = 1;
	} else {
		data->pwmctl[index] = 2;

		switch (v) {
		case 0:
			data->pwmchan[index] = 1;
			break;
		case 1:
			data->pwmchan[index] = 2;
			break;
		case 2:
			data->pwmchan[index] = 4;
			break;
		case 5:
			data->pwmchan[index] = 6;
			break;
		case 6:
			data->pwmchan[index] = 7;
			break;
		}
	}
}

static int adt7475_update_measure(struct device *dev)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	u16 ext;
	int i;
	int ret;

	ret = adt7475_read(REG_STATUS2);
	if (ret < 0)
		return ret;
	data->alarms = ret << 8;

	ret = adt7475_read(REG_STATUS1);
	if (ret < 0)
		return ret;
	data->alarms |= ret;

	ret = adt7475_read(REG_EXTEND2);
	if (ret < 0)
		return ret;

	ext = (ret << 8);

	ret = adt7475_read(REG_EXTEND1);
	if (ret < 0)
		return ret;

	ext |= ret;

	for (i = 0; i < ADT7475_VOLTAGE_COUNT; i++) {
		if (!(data->has_voltage & (1 << i)))
			continue;
		ret = adt7475_read(VOLTAGE_REG(i));
		if (ret < 0)
			return ret;
		data->voltage[INPUT][i] =
			(ret << 2) |
			((ext >> (i * 2)) & 3);
	}

	for (i = 0; i < ADT7475_TEMP_COUNT; i++) {
		ret = adt7475_read(TEMP_REG(i));
		if (ret < 0)
			return ret;
		data->temp[INPUT][i] =
			(ret << 2) |
			((ext >> ((i + 5) * 2)) & 3);
	}

	if (data->has_voltage & (1 << 5)) {
		ret = adt7475_read(REG_STATUS4);
		if (ret < 0)
			return ret;
		data->alarms |= ret << 24;

		ret = adt7475_read(REG_EXTEND3);
		if (ret < 0)
			return ret;
		ext = ret;

		ret = adt7475_read(REG_VTT);
		if (ret < 0)
			return ret;
		data->voltage[INPUT][5] = ret << 2 |
			((ext >> 4) & 3);
	}

	if (data->has_voltage & (1 << 6)) {
		ret = adt7475_read(REG_STATUS4);
		if (ret < 0)
			return ret;
		data->alarms |= ret << 24;

		ret = adt7475_read(REG_EXTEND3);
		if (ret < 0)
			return ret;
		ext = ret;

		ret = adt7475_read(REG_IMON);
		if (ret < 0)
			return ret;
		data->voltage[INPUT][6] = ret << 2 |
			((ext >> 6) & 3);
	}

	for (i = 0; i < ADT7475_TACH_COUNT; i++) {
		if (i == 3 && !data->has_fan4)
			continue;
		ret = adt7475_read_word(client, TACH_REG(i));
		if (ret < 0)
			return ret;
		data->tach[INPUT][i] = ret;
	}

	/* Updated by hw when in auto mode */
	for (i = 0; i < ADT7475_PWM_COUNT; i++) {
		if (i == 1 && !data->has_pwm2)
			continue;
		ret = adt7475_read(PWM_REG(i));
		if (ret < 0)
			return ret;
		data->pwm[INPUT][i] = ret;
	}

	if (data->has_vid) {
		ret = adt7475_read(REG_VID);
		if (ret < 0)
			return ret;
		data->vid = ret & 0x3f;
	}

	return 0;
}

static struct adt7475_data *adt7475_update_device(struct device *dev)
{
	struct adt7475_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);

	/* Measurement values update every 2 seconds */
	if (time_after64(get_jiffies_64(), data->measure_updated + HZ * 2) ||
	    !data->valid) {
		ret = adt7475_update_measure(dev);
		if (ret) {
			data->valid = false;
			mutex_unlock(&data->lock);
			return ERR_PTR(ret);
		}
		data->measure_updated = get_jiffies_64();
		data->valid = true;
	}

	mutex_unlock(&data->lock);

	return data;
}

module_i2c_driver(adt7475_driver);

MODULE_AUTHOR("Advanced Micro Devices, Inc");
MODULE_DESCRIPTION("adt7475 driver");
MODULE_LICENSE("GPL");
