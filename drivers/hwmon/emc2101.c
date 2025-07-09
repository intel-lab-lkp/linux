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
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
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
#define  CONV_RATE_SHIFT		0
#define  CONV_RATE_16000		0
#define  CONV_RATE_8000			1
#define  CONV_RATE_4000			2
#define  CONV_RATE_2000			3
#define  CONV_RATE_1000			4
#define  CONV_RATE_500			5
#define  CONV_RATE_250			6
#define  CONV_RATE_125			7
#define  CONV_RATE_62			8
#define  CONV_RATE_31			9
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
#define  EXT_IDEALITY_SHIFT		0
#define  EXT_IDEALITY_START		9846
#define  EXT_IDEALITY_STEP		13
#define  EXT_IDEALITY_VAL(x)		(EXT_IDEALITY_START + \
					 ((x) * EXT_IDEALITY_STEP))
#define  EXT_IDEALITY_MASK		0x3f
#define REG_BETA_COMP			0x18
#define  BETA_COMP_AUTO			BIT(3)
#define  BETA_COMP_SHIFT		0
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
/* Can only be written once */
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
#define  TACH_FALSE_READ_SHIFT		0
#define  TACH_FALSE_READ_ENABLE		0
#define  TACH_FALSE_READ_DISABLE	3
#define  TACH_FALSE_READ_MASK		0x3
#define REG_FAN_SPIN			0x4b
#define  FAN_SPIN_UP_ABORT		BIT(5)
#define  FAN_SPIN_UP_POWER_SHIFT	3
#define  FAN_SPIN_UP_POWER_100		3
#define  FAN_SPIN_UP_POWER_75		2
#define  FAN_SPIN_UP_POWER_50		1
#define  FAN_SPIN_UP_POWER_0		0
#define  FAN_SPIN_UP_POWER_MASK		0x3
#define  FAN_SPIN_UP_TIME_SHIFT		0
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
#define  FAN_SET_SHIFT			0
#define  FAN_SET_MASK			0x3f
#define REG_PWM_FREQ			0x4d
#define  PWM_FREQ_SHIFT			0
#define  PWM_FREQ_MASK			0x1f
#define REG_PWM_FREQ_DIV		0x4e
#define REG_FAN_LUT_HYST		0x4f
#define  FAN_LUT_HYST_SHIFT		0
#define  FAN_LUT_HYST_MASK		0x1f
#define REG_FAN_LUT_TEMP(x)		(0x50 + (0x2 * (x)))
/* Write only with FAN_LUT_DISABLE */
#define  FAN_LUT_TEMP_SHIFT		0
#define  FAN_LUT_TEMP_MASK		0x7f
#define REG_FAN_LUT_SPEED(x)		(0x51 + (0x2 * (x)))
/* Write only with FAN_LUT_DISABLE */
#define  FAN_LUT_SPEED_SHIFT		0
#define  FAN_LUT_SPEED_MASK		0x3f
#define REG_AVG_FILTER			0xbf
#define  FILTER_SHIFT			1
#define  FILTER_L2			3
#define  FILTER_L1			1
#define  FILTER_NONE			0
#define  FILTER_MASK			0x3
#define  ALERT_PIN_TEMP_COMP		BIT(0)
#define REG_PRODUCT_ID			0xfd
#define REG_MANUFACTURER_ID		0xfe
#define REG_REVISION			0xff

#define CLK_FREQ_ALT			1400
#define CLK_FREQ_BASE			360000

#define FAN_LUT_COUNT			8
#define FAN_LUT_HYST_MIN		0
#define FAN_LUT_HYST_MAX		31
#define FAN_MIN_READ			0xffff
#define FAN_RPM_FACTOR			5400000

#define MANUFACTURER_ID			0x5d

#define PWM_MASK			0x3f

#define TEMP_FAULT_OPEN			0x7f00
#define TEMP_FAULT_SHORT		0x7fe0
#define TEMP_LO_FRAC			125
#define TEMP_LO_SHIFT			5
#define TEMP_LO_MASK			0x7

#define TEMP_MIN			-64
#define TEMP_MAX			127
#define TEMP_MAX_FRAC			750

enum emc2101_auto_channels_temp {
	EMC2101_ACT_EXT = 2,
	EMC2101_ACT_FORCE = 3
};

enum emc2101_mode {
	EMC2101_MODE_PWM = 0,
	EMC2101_MODE_DAC = 1
};

enum ecm2101_product_id {
	EMC2101 = 0x16,
	EMC2101_R = 0x28
};

enum emc2101_pwm {
	EMC2101_PWM_MANUAL = 1,
	EMC2101_PWM_LUT = 2
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

enum emc2101_fields {
	/* BETA_COMP */
	F_BETA_COMP,
	F_BETA_COMP_AUTO,

	/* CONFIG */
	F_TEMP_EXT_CRIT_UNLOCK,
	F_PIN_FUNC_TACH,
	F_SMBUS_TOUT_DISABLE,
	F_FAN_MODE_DAC,
	F_FAN_STBY,
	F_STBY_MODE,

	/* CONV_RATE */
	F_CONV_RATE,

	/* EXT_IDEALITY */
	F_EXT_IDEALITY,

	/* FAN_CONFIG */
	F_TACH_FALSE_READ,
	F_FAN_CLK_OVR,
	F_FAN_CLK_SEL,
	F_FAN_POL_INV,
	F_FAN_LUT_DISABLE,
	F_FAN_EXT_TEMP_FORCE,

	/* FAN_LUT */
	F_FAN_LUT_HYST,
	F_FAN_LUT_SPEED_1,
	F_FAN_LUT_SPEED_2,
	F_FAN_LUT_SPEED_3,
	F_FAN_LUT_SPEED_4,
	F_FAN_LUT_SPEED_5,
	F_FAN_LUT_SPEED_6,
	F_FAN_LUT_SPEED_7,
	F_FAN_LUT_SPEED_8,
	F_FAN_LUT_TEMP_1,
	F_FAN_LUT_TEMP_2,
	F_FAN_LUT_TEMP_3,
	F_FAN_LUT_TEMP_4,
	F_FAN_LUT_TEMP_5,
	F_FAN_LUT_TEMP_6,
	F_FAN_LUT_TEMP_7,
	F_FAN_LUT_TEMP_8,

	/* FAN_SET */
	F_FAN_SET,

	/* FAN_SPIN */
	F_SPIN_UP_TIME,
	F_SPIN_UP_POWER,
	F_SPIN_UP_ABORT,

	/* PWM_FREQ */
	F_PWM_FREQ,
	F_PWM_FREQ_DIV,

	/* STATUS */
	F_TACH_LOW_ALARM,
	F_TEMP_EXT_CRIT_ALARM,
	F_TEMP_EXT_FAULT,
	F_TEMP_EXT_LOW_ALARM,
	F_TEMP_EXT_HIGH_ALARM,
	F_TEMP_INT_HIGH_ALARM,

	/* TEMP_INT */
	F_TEMP_INT,
	F_TEMP_INT_MAX,

	/* TEMP_EXT */
	F_TEMP_EXT_CRIT,
	F_TEMP_EXT_CRIT_HYST,

	/* TEMP_EXT_FORCE */
	F_TEMP_EXT_FORCE,

	/* sentinel */
	F_MAX_FIELDS
};

#define F_FAN_LUT_SPEED(x) (F_FAN_LUT_SPEED_1 + (x))
#define F_FAN_LUT_TEMP(x) (F_FAN_LUT_TEMP_1 + (x))

static const struct reg_field emc2101_reg_fields[] = {
	/* BETA_COMP */
	[F_BETA_COMP] = REG_FIELD(REG_BETA_COMP, 0, 2),
	[F_BETA_COMP_AUTO] = REG_FIELD(REG_BETA_COMP, 3, 3),

	/* CONFIG */
	[F_TEMP_EXT_CRIT_UNLOCK] = REG_FIELD(REG_CONFIG, 1, 1),
	[F_PIN_FUNC_TACH] = REG_FIELD(REG_CONFIG, 2, 2),
	[F_SMBUS_TOUT_DISABLE] = REG_FIELD(REG_CONFIG, 3, 3),
	[F_FAN_MODE_DAC] = REG_FIELD(REG_CONFIG, 4, 4),
	[F_FAN_STBY] = REG_FIELD(REG_CONFIG, 5, 5),
	[F_STBY_MODE] = REG_FIELD(REG_CONFIG, 6, 6),

	/* CONV_RATE */
	[F_CONV_RATE] = REG_FIELD(REG_CONV_RATE, 0, 3),

	/* EXT_IDEALITY */
	[F_EXT_IDEALITY] = REG_FIELD(REG_EXT_IDEALITY, 0, 5),

	/* FAN_CONFIG */
	[F_TACH_FALSE_READ] = REG_FIELD(REG_FAN_CONFIG, 0, 1),
	[F_FAN_CLK_OVR] = REG_FIELD(REG_FAN_CONFIG, 2, 2),
	[F_FAN_CLK_SEL] = REG_FIELD(REG_FAN_CONFIG, 3, 3),
	[F_FAN_POL_INV] = REG_FIELD(REG_FAN_CONFIG, 4, 4),
	[F_FAN_LUT_DISABLE] = REG_FIELD(REG_FAN_CONFIG, 5, 5),
	[F_FAN_EXT_TEMP_FORCE] = REG_FIELD(REG_FAN_CONFIG, 6, 6),

	/* FAN_LUT */
	[F_FAN_LUT_HYST] = REG_FIELD(REG_FAN_LUT_HYST, 0, 4),
	[F_FAN_LUT_SPEED_1] = REG_FIELD(REG_FAN_LUT_SPEED(0), 0, 5),
	[F_FAN_LUT_SPEED_2] = REG_FIELD(REG_FAN_LUT_SPEED(1), 0, 5),
	[F_FAN_LUT_SPEED_3] = REG_FIELD(REG_FAN_LUT_SPEED(2), 0, 5),
	[F_FAN_LUT_SPEED_4] = REG_FIELD(REG_FAN_LUT_SPEED(3), 0, 5),
	[F_FAN_LUT_SPEED_5] = REG_FIELD(REG_FAN_LUT_SPEED(4), 0, 5),
	[F_FAN_LUT_SPEED_6] = REG_FIELD(REG_FAN_LUT_SPEED(5), 0, 5),
	[F_FAN_LUT_SPEED_7] = REG_FIELD(REG_FAN_LUT_SPEED(6), 0, 5),
	[F_FAN_LUT_SPEED_8] = REG_FIELD(REG_FAN_LUT_SPEED(7), 0, 5),
	[F_FAN_LUT_TEMP_1] = REG_FIELD(REG_FAN_LUT_TEMP(0), 0, 6),
	[F_FAN_LUT_TEMP_2] = REG_FIELD(REG_FAN_LUT_TEMP(1), 0, 6),
	[F_FAN_LUT_TEMP_3] = REG_FIELD(REG_FAN_LUT_TEMP(2), 0, 6),
	[F_FAN_LUT_TEMP_4] = REG_FIELD(REG_FAN_LUT_TEMP(3), 0, 6),
	[F_FAN_LUT_TEMP_5] = REG_FIELD(REG_FAN_LUT_TEMP(4), 0, 6),
	[F_FAN_LUT_TEMP_6] = REG_FIELD(REG_FAN_LUT_TEMP(5), 0, 6),
	[F_FAN_LUT_TEMP_7] = REG_FIELD(REG_FAN_LUT_TEMP(6), 0, 6),
	[F_FAN_LUT_TEMP_8] = REG_FIELD(REG_FAN_LUT_TEMP(7), 0, 6),

	/* FAN_SET */
	[F_FAN_SET] = REG_FIELD(REG_FAN_SET, 0, 5),

	/* FAN_SPIN */
	[F_SPIN_UP_TIME] = REG_FIELD(REG_FAN_SPIN, 0, 2),
	[F_SPIN_UP_POWER] = REG_FIELD(REG_FAN_SPIN, 3, 4),
	[F_SPIN_UP_ABORT] = REG_FIELD(REG_FAN_SPIN, 5, 5),

	/* PWM_FREQ */
	[F_PWM_FREQ] = REG_FIELD(REG_PWM_FREQ, 0, 4),
	[F_PWM_FREQ_DIV] = REG_FIELD(REG_PWM_FREQ_DIV, 0, 7),

	/* STATUS */
	[F_TACH_LOW_ALARM] = REG_FIELD(REG_STATUS, 0, 0),
	[F_TEMP_EXT_CRIT_ALARM] = REG_FIELD(REG_STATUS, 1, 1),
	[F_TEMP_EXT_FAULT] = REG_FIELD(REG_STATUS, 2, 2),
	[F_TEMP_EXT_LOW_ALARM] = REG_FIELD(REG_STATUS, 3, 3),
	[F_TEMP_EXT_HIGH_ALARM] = REG_FIELD(REG_STATUS, 4, 4),
	[F_TEMP_INT_HIGH_ALARM] = REG_FIELD(REG_STATUS, 6, 6),

	/* TEMP_INT */
	[F_TEMP_INT] = REG_FIELD(REG_TEMP_INT, 0, 7),
	[F_TEMP_INT_MAX] = REG_FIELD(REG_TEMP_INT_MAX, 0, 6),

	/* TEMP_EXT */
	[F_TEMP_EXT_CRIT] = REG_FIELD(REG_TEMP_EXT_CRIT, 0, 6),
	[F_TEMP_EXT_CRIT_HYST] = REG_FIELD(REG_TEMP_EXT_CRIT_HYST, 0, 6),

	/* TEMP_EXT_FORCE */
	[F_TEMP_EXT_FORCE] = REG_FIELD(REG_TEMP_EXT_FORCE, 0, 7),
};

struct emc2101_data {
	struct regmap *regmap;
	struct regmap_field *fields[F_MAX_FIELDS];
	struct device *dev;
	struct mutex mutex; /* serializes FAN_LUT_DISABLE and TEMP_EXT_CRIT_UNLOCK */
};

static const u16 emc2101_conv_time[] = {
	16000, 8000, 4000, 2000, 1000, 500, 250, 125, 62, 31
};

static const u8 emc2101_fan_spin_up_power[] = {
	0, 50, 75, 100
};

static const u16 emc2101_fan_spin_up_time[] = {
	0, 50, 100, 200, 400, 800, 1600, 3200
};

static const unsigned int regs_tach[2] = {REG_TACH_HI, REG_TACH_LO};
static const unsigned int regs_tach_min[2] = {REG_TACH_MIN_HI, REG_TACH_MIN_LO};
static const unsigned int regs_temp_ext[2] = {REG_TEMP_EXT_HI, REG_TEMP_EXT_LO};
static const unsigned int regs_temp_ext_max[2] = {REG_TEMP_EXT_MAX_HI, REG_TEMP_EXT_MAX_LO};
static const unsigned int regs_temp_ext_min[2] = {REG_TEMP_EXT_MIN_HI, REG_TEMP_EXT_MIN_LO};

static inline int emc2101_read_u16(struct emc2101_data *data, const unsigned int *regs, u16 *val)
{
	u8 read_seq[2];
	int ret;

	ret = regmap_multi_reg_read(data->regmap, regs, read_seq, 2);
	if (!ret) {
		*val = (read_seq[0] & 0xff) << 8;
		*val |= read_seq[1] & 0xff;
	}

	return ret;
}

static inline int emc2101_write_u16(struct emc2101_data *data, const unsigned int *regs, u16 val)
{
	const struct reg_sequence write_seq[2] = {
		{ regs[0], (val >> 8) & 0xff },
		{ regs[1], val & 0xff },
	};

	return regmap_multi_reg_write(data->regmap, write_seq, 2);
}

static inline u16 emc2101_rpm_to_u16(long rpm)
{
	u16 val;

	if (rpm > 0)
		val = clamp_val(FAN_RPM_FACTOR / rpm, 1, FAN_MIN_READ);
	else
		val = FAN_MIN_READ;

	return val;
}

static inline long emc2101_u16_to_rpm(u16 val)
{
	long rpm;

	val = clamp_val(val, 1, FAN_MIN_READ);
	if (val < FAN_MIN_READ)
		rpm = FAN_RPM_FACTOR / val;
	else
		rpm = 0;

	return rpm;
}

static inline u16 emc2101_temp_to_u16(long temp)
{
	s8 val_hi = clamp_val(temp / 1000, TEMP_MIN, TEMP_MAX);
	long rem = temp % 1000;
	u8 val_lo;

	if (val_hi == TEMP_MIN)
		rem = 0;
	else if (val_hi == TEMP_MAX)
		rem = TEMP_MAX_FRAC;

	if (rem < 0) {
		val_hi -= 1;
		rem = (1000 + rem);
	}

	rem /= TEMP_LO_FRAC;
	val_lo = (rem & TEMP_LO_MASK) << TEMP_LO_SHIFT;

	return (val_hi << 8) | val_lo;
}

static inline long emc2101_u16_to_temp(u16 val)
{
	const s8 val_hi = (val >> 8) & 0xff;
	const u8 val_lo = (val >> TEMP_LO_SHIFT) & TEMP_LO_MASK;
	long temp;

	temp = val_hi * 1000;
	temp += val_lo * TEMP_LO_FRAC;

	return temp;
}

static inline bool emc2101_lut_edit(struct emc2101_data *data, bool *disabled)
{
	unsigned int lut_disabled;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_LUT_DISABLE], &lut_disabled);
	if (ret)
		return ret;

	*disabled = lut_disabled;

	return regmap_field_write(data->fields[F_FAN_LUT_DISABLE], 1);
}

static inline bool emc2101_lut_restore(struct emc2101_data *data, bool disabled)
{
	if (!disabled)
		return regmap_field_write(data->fields[F_FAN_LUT_DISABLE], 0);

	return 0;
}

static int emc2101_lut_hyst_write(struct regmap_field *field, long temp)
{
	const unsigned int val = clamp_val(temp / 1000, FAN_LUT_HYST_MIN, FAN_LUT_HYST_MAX);

	return regmap_field_write(field, val);
}

static int emc2101_temp_neg_read(struct regmap_field *field, long *temp)
{
	unsigned int val;
	int ret;

	ret = regmap_field_read(field, &val);
	if (!ret)
		*temp = ((s8) val) * 1000;

	return ret;
}

static int emc2101_temp_neg_write(struct regmap_field *field, long temp)
{
	const s8 val = clamp_val(temp / 1000, TEMP_MIN, TEMP_MAX);

	return regmap_field_write(field, val);
}

static int emc2101_temp_pos_read(struct regmap_field *field, long *temp)
{
	unsigned int val;
	int ret;

	ret = regmap_field_read(field, &val);
	if (!ret)
		*temp = val * 1000;

	return ret;
}

static int emc2101_temp_pos_write(struct regmap_field *field, long temp)
{
	const u8 val = clamp_val(temp / 1000, 0, TEMP_MAX);

	return regmap_field_write(field, val);
}

static int emc2101_temp_frac_read(struct emc2101_data *data, const unsigned int *regs, long *temp)
{
	u16 temp_frac;
	int ret;

	ret = emc2101_read_u16(data, regs, &temp_frac);
	if (ret)
		return ret;

	switch (temp_frac) {
	case TEMP_FAULT_OPEN:
		dev_warn(data->dev, "[%02x, %02x]: diode fault (open)", regs[0], regs[1]);
		return -ENODATA;
	case TEMP_FAULT_SHORT:
		dev_warn(data->dev, "[%02x, %02x]: diode fault (short)", regs[0], regs[1]);
		return -ENODATA;
	default:
		break;
	}

	*temp = emc2101_u16_to_temp(temp_frac);

	return ret;
}

static int emc2101_temp_frac_write(struct emc2101_data *data, const unsigned int *regs, long temp)
{
	long temp_frac = emc2101_temp_to_u16(temp);

	return emc2101_write_u16(data, regs, temp_frac);
}

static int emc2101_pwm_write(struct regmap_field *field, long pwm)
{
	const unsigned int val = clamp_val(pwm, 0, PWM_MASK);

	return regmap_field_write(field, val);
}

static ssize_t fan_spin_up_abort_show(struct device *dev, struct device_attribute *devattr,
				      char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_abort;
	int ret;

	ret = regmap_field_read(data->fields[F_SPIN_UP_ABORT], &fan_spin_abort);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", fan_spin_abort);
}

static ssize_t fan_spin_up_abort_store(struct device *dev, struct device_attribute *devattr,
				       const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_abort;
	int ret;

	ret = kstrtouint(buf, 10, &fan_spin_abort);
	if (ret)
		return ret;

	switch (fan_spin_abort) {
	case 0:
	case 1:
		ret = regmap_field_write(data->fields[F_SPIN_UP_ABORT], fan_spin_abort);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return !ret ? count : ret;
}

static ssize_t fan_spin_up_time_show(struct device *dev, struct device_attribute *devattr,
				     char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_time;
	int ret;

	ret = regmap_field_read(data->fields[F_SPIN_UP_TIME], &fan_spin_time);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", emc2101_fan_spin_up_time[fan_spin_time]);
}

static ssize_t fan_spin_up_time_store(struct device *dev, struct device_attribute *devattr,
				      const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_time, val;
	int ret;

	ret = kstrtouint(buf, 10, &fan_spin_time);
	if (ret)
		return ret;

	val = find_closest(fan_spin_time, emc2101_fan_spin_up_time,
			   ARRAY_SIZE(emc2101_fan_spin_up_time));

	ret = regmap_field_write(data->fields[F_SPIN_UP_TIME], val);

	return !ret ? count : ret;
}

static ssize_t fan_spin_up_power_show(struct device *dev, struct device_attribute *devattr,
				      char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_power;
	int ret;

	ret = regmap_field_read(data->fields[F_SPIN_UP_POWER], &fan_spin_power);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", emc2101_fan_spin_up_power[fan_spin_power]);
}

static ssize_t fan_spin_up_power_store(struct device *dev, struct device_attribute *devattr,
				       const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_spin_power, val;
	int ret;

	ret = kstrtouint(buf, 10, &fan_spin_power);
	if (ret)
		return ret;

	val = find_closest(fan_spin_power, emc2101_fan_spin_up_power,
			   ARRAY_SIZE(emc2101_fan_spin_up_power));

	ret = regmap_field_write(data->fields[F_SPIN_UP_POWER], val);

	return !ret ? count : ret;
}

static ssize_t fan_standby_show(struct device *dev, struct device_attribute *devattr,
				char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_standby;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_STBY], &fan_standby);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", fan_standby);
}

static ssize_t fan_standby_store(struct device *dev, struct device_attribute *devattr,
				 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_standby;
	int ret;

	ret = kstrtouint(buf, 10, &fan_standby);
	if (ret)
		return ret;

	switch (fan_standby) {
	case 0:
	case 1:
		ret = regmap_field_write(data->fields[F_FAN_STBY], fan_standby);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_pwm_show(struct device *dev, struct device_attribute *devattr,
				       char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_pwm;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_LUT_SPEED(attr->index)], &lut_pwm);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", lut_pwm);
}

static ssize_t __pwm_auto_point_pwm_store(struct emc2101_data *data,
					  struct device_attribute *devattr, unsigned int lut_pwm)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	bool lut_disable;
	int ret;

	ret = emc2101_lut_edit(data, &lut_disable);
	if (ret)
		return ret;

	ret = emc2101_pwm_write(data->fields[F_FAN_LUT_SPEED(attr->index)], lut_pwm);
	if (ret)
		return ret;

	return emc2101_lut_restore(data, lut_disable);
}

static ssize_t pwm_auto_point_pwm_store(struct device *dev, struct device_attribute *devattr,
					const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_pwm;
	int ret;

	ret = kstrtouint(buf, 10, &lut_pwm);
	if (ret)
		return ret;

	mutex_lock(&data->mutex);
	ret = __pwm_auto_point_pwm_store(data, devattr, lut_pwm);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_temp_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct emc2101_data *data = dev_get_drvdata(dev);
	long lut_temp;
	int ret;

	ret = emc2101_temp_pos_read(data->fields[F_FAN_LUT_TEMP(attr->index)], &lut_temp);
	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", lut_temp);
}

static ssize_t __pwm_auto_point_temp_store(struct emc2101_data *data,
					   struct device_attribute *devattr, unsigned int lut_temp)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	bool lut_disable;
	unsigned int i;
	long cur_temp;
	int ret;

	ret = emc2101_lut_edit(data, &lut_disable);
	if (ret)
		return ret;

	for (i = 0; i < FAN_LUT_COUNT; i++) {
		struct regmap_field *field = data->fields[F_FAN_LUT_TEMP(i)];

		ret = emc2101_temp_pos_read(field, &cur_temp);
		if (ret)
			return ret;

		if (i < attr->index) {
			if (cur_temp > lut_temp)
				ret = emc2101_temp_pos_write(field, lut_temp);
		} else if (i > attr->index) {
			if (cur_temp < lut_temp)
				ret = emc2101_temp_pos_write(field, lut_temp);
		} else {
			ret = emc2101_temp_pos_write(field, lut_temp);
		}

		if (ret)
			return ret;
	}

	return emc2101_lut_restore(data, lut_disable);
}

static ssize_t pwm_auto_point_temp_store(struct device *dev, struct device_attribute *devattr,
					 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_temp;
	int ret;

	ret = kstrtouint(buf, 10, &lut_temp);
	if (ret)
		return ret;

	mutex_lock(&data->mutex);
	ret = __pwm_auto_point_temp_store(data, devattr, lut_temp);
	mutex_unlock(&data->mutex);

	return !ret ? count : ret;
}

static ssize_t pwm_auto_point_temp_hyst_show(struct device *dev, struct device_attribute *devattr,
					     char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long lut_hyst;
	int ret;

	ret = emc2101_temp_pos_read(data->fields[F_FAN_LUT_HYST], &lut_hyst);
	if (ret)
		return ret;

	return sprintf(buf, "%lu\n", lut_hyst);
}

static ssize_t pwm_auto_point_temp_hyst_store(struct device *dev, struct device_attribute *devattr,
					      const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_hyst;
	int ret;

	ret = kstrtouint(buf, 10, &lut_hyst);
	if (ret)
		return ret;

	ret = emc2101_lut_hyst_write(data->fields[F_FAN_LUT_HYST], lut_hyst);

	return !ret ? count : ret;
}

static ssize_t pwm_polarity_invert_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int polarity_inverted;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_POL_INV], &polarity_inverted);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", polarity_inverted);
}

static ssize_t pwm_polarity_invert_store(struct device *dev, struct device_attribute *devattr,
					 const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int polarity_inverted;
	int ret;

	ret = kstrtouint(buf, 10, &polarity_inverted);
	if (ret)
		return ret;

	switch (polarity_inverted) {
	case 0:
	case 1:
		ret = regmap_field_write(data->fields[F_FAN_POL_INV], polarity_inverted);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return !ret ? count : ret;
}

static ssize_t temp_external_force_show(struct device *dev, struct device_attribute *devattr,
					char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	long temp_force;
	int ret;

	ret = emc2101_temp_neg_read(data->fields[F_TEMP_EXT_FORCE], &temp_force);
	if (ret)
		return ret;

	return sprintf(buf, "%ld\n", temp_force);
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

	ret = emc2101_temp_neg_write(data->fields[F_TEMP_EXT_FORCE], temp_force);

	return !ret ? count : ret;
}

static ssize_t temp_external_ideality_show(struct device *dev, struct device_attribute *devattr,
					   char *buf)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int ext_ideality;
	int ret;

	ret = regmap_field_read(data->fields[F_EXT_IDEALITY], &ext_ideality);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", EXT_IDEALITY_VAL(ext_ideality));
}

static ssize_t temp_external_ideality_store(struct device *dev, struct device_attribute *devattr,
					    const char *buf, size_t count)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int ext_ideality_factor, val;
	int ret;

	ret = kstrtouint(buf, 10, &ext_ideality_factor);
	if (ret)
		return ret;

	ext_ideality_factor = clamp_val(ext_ideality_factor, EXT_IDEALITY_START,
					EXT_IDEALITY_VAL(EXT_IDEALITY_MASK));
	val = (ext_ideality_factor - EXT_IDEALITY_START) / EXT_IDEALITY_STEP;

	ret = regmap_field_write(data->fields[F_EXT_IDEALITY], val);

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
	unsigned int conv_rate;
	int ret;

	ret = regmap_field_read(data->fields[F_CONV_RATE], &conv_rate);
	if (!ret) {
		if (conv_rate < ARRAY_SIZE(emc2101_conv_time))
			*val = emc2101_conv_time[conv_rate];
		else
			*val = emc2101_conv_time[CONV_RATE_31];
	}

	return ret;
}

static int emc2101_chip_update_interval_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int conv_rate;

	conv_rate = find_closest_descending(val, emc2101_conv_time, ARRAY_SIZE(emc2101_conv_time));

	return regmap_field_write(data->fields[F_CONV_RATE], conv_rate);
}

static int emc2101_fan_div_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int pwm_freq_div;
	int ret;

	ret = regmap_field_read(data->fields[F_PWM_FREQ_DIV], &pwm_freq_div);
	if (!ret)
		*val = pwm_freq_div;

	return ret;
}

static int emc2101_fan_div_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	struct regmap_field *field = data->fields[F_PWM_FREQ_DIV];
	unsigned int pwm_freq_div;

	pwm_freq_div = clamp_val(val, 1, 0xff);

	return regmap_field_write(field, pwm_freq_div);
}

static int emc2101_fan_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count;
	int ret;

	ret = emc2101_read_u16(data, regs_tach, &tach_count);
	if (ret)
		return ret;

	*val = emc2101_u16_to_rpm(tach_count);

	return 0;
}

static int emc2101_fan_min_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count;
	int ret;

	ret = emc2101_read_u16(data, regs_tach_min, &tach_count);
	if (ret)
		return ret;

	*val = emc2101_u16_to_rpm(tach_count);

	return 0;
}

static int emc2101_fan_min_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	u16 tach_count = emc2101_rpm_to_u16(val);

	return emc2101_write_u16(data, regs_tach_min, tach_count);
}

static int emc2101_fan_min_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int tach_low;
	int ret;

	ret = regmap_field_read(data->fields[F_TACH_LOW_ALARM], &tach_low);
	if (ret)
		return ret;

	*val = tach_low;

	return 0;
}

static int emc2101_pwm_auto_channels_temp_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int temp_ext_force;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_EXT_TEMP_FORCE], &temp_ext_force);
	if (ret)
		return ret;

	*val = temp_ext_force ? EMC2101_ACT_FORCE : EMC2101_ACT_EXT;

	return 0;
}

static int emc2101_pwm_auto_channels_temp_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	switch (val) {
	case EMC2101_ACT_EXT:
		ret = regmap_field_write(data->fields[F_FAN_EXT_TEMP_FORCE], 0);
		break;
	case EMC2101_ACT_FORCE:
		ret = regmap_field_write(data->fields[F_FAN_EXT_TEMP_FORCE], 1);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int emc2101_pwm_enable_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_disable;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_LUT_DISABLE], &lut_disable);
	if (ret)
		return ret;

	*val = lut_disable ? EMC2101_PWM_MANUAL : EMC2101_PWM_LUT;

	return 0;
}

static int emc2101_pwm_enable_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int lut_disable;
	int ret;

	switch (val) {
	case EMC2101_PWM_MANUAL:
		lut_disable = 1;
		break;
	case EMC2101_PWM_LUT:
		lut_disable = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->mutex);
	ret = regmap_field_write(data->fields[F_FAN_LUT_DISABLE], lut_disable);
	mutex_unlock(&data->mutex);

	return ret;
}

static int emc2101_pwm_freq_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_clk_ovr, fan_clk_sel;
	unsigned int pwm_freq, pwm_freq_div;
	unsigned int base_clk, div;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_CLK_OVR], &fan_clk_ovr);
	if (ret)
		return ret;

	ret = regmap_field_read(data->fields[F_FAN_CLK_SEL], &fan_clk_sel);
	if (ret)
		return ret;

	ret = regmap_field_read(data->fields[F_PWM_FREQ], &pwm_freq);
	if (ret)
		return ret;

	if (fan_clk_ovr) {
		ret = regmap_field_read(data->fields[F_PWM_FREQ_DIV], &pwm_freq_div);
		if (ret)
			return ret;
	} else {
		pwm_freq_div = 1;
	}

	if (fan_clk_sel)
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

static int emc2101_pwm_freq_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_clk_ovr, fan_clk_sel;
	unsigned int pwm_freq, pwm_freq_div;
	unsigned int base_clk;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_CLK_OVR], &fan_clk_ovr);
	if (ret)
		return ret;

	ret = regmap_field_read(data->fields[F_FAN_CLK_SEL], &fan_clk_sel);
	if (ret)
		return ret;

	if (fan_clk_ovr) {
		ret = regmap_field_read(data->fields[F_PWM_FREQ_DIV], &pwm_freq_div);
		if (ret)
			return ret;
	} else {
		pwm_freq_div = 1;
	}

	if (fan_clk_sel)
		base_clk = CLK_FREQ_ALT;
	else
		base_clk = CLK_FREQ_BASE;

	pwm_freq = base_clk / (2 * pwm_freq_div * val);

	return emc2101_pwm_write(data->fields[F_PWM_FREQ], pwm_freq);
}

static int emc2101_pwm_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_set;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_SET], &fan_set);
	if (!ret)
		*val = fan_set;

	return ret;
}

static int emc2101_pwm_input_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_pwm_write(data->fields[F_FAN_SET], val);
}

static int emc2101_pwm_mode_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int fan_mode_dac;
	int ret;

	ret = regmap_field_read(data->fields[F_FAN_MODE_DAC], &fan_mode_dac);
	if (ret)
		return ret;

	*val = fan_mode_dac ? EMC2101_MODE_DAC : EMC2101_MODE_PWM;

	return ret;
}

static int emc2101_pwm_mode_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	int ret;

	switch (val) {
	case EMC2101_MODE_DAC:
	case EMC2101_MODE_PWM:
		ret = regmap_field_write(data->fields[F_FAN_MODE_DAC], val);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int emc2101_temp_ext_crit_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int temp_ext_crit;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_EXT_CRIT_ALARM], &temp_ext_crit);
	if (ret)
		return ret;

	*val = temp_ext_crit;

	return 0;
}

static int emc2101_temp_ext_crit_hyst_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_pos_read(data->fields[F_TEMP_EXT_CRIT_HYST], val);
}

static int emc2101_temp_ext_crit_hyst_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_pos_write(data->fields[F_TEMP_EXT_CRIT_HYST], val);
}

static int emc2101_temp_ext_crit_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_pos_read(data->fields[F_TEMP_EXT_CRIT], val);
}

static int __emc2101_temp_ext_crit_write(struct emc2101_data *data, long val)
{
	unsigned int temp_ext_crit_unlock;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_EXT_CRIT_UNLOCK], &temp_ext_crit_unlock);
	if (ret)
		return ret;

	if (temp_ext_crit_unlock) {
		dev_err(data->dev, "critical temperature can only be updated once");
		return -EIO;
	}

	ret = regmap_field_write(data->fields[F_TEMP_EXT_CRIT_UNLOCK], 1);
	if (ret)
		return ret;

	return emc2101_temp_pos_write(data->fields[F_TEMP_EXT_CRIT], val);
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
	unsigned int temp_ext_fault;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_EXT_FAULT], &temp_ext_fault);
	if (ret)
		return ret;

	*val = temp_ext_fault;

	return 0;
}

static int emc2101_temp_ext_max_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int temp_ext_high;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_EXT_HIGH_ALARM], &temp_ext_high);
	if (ret)
		return ret;

	*val = temp_ext_high;

	return 0;
}

static int emc2101_temp_ext_max_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_read(data, regs_temp_ext_max, val);
}

static int emc2101_temp_ext_max_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_write(data, regs_temp_ext_max, val);
}

static int emc2101_temp_ext_min_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int temp_ext_low;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_EXT_LOW_ALARM], &temp_ext_low);
	if (ret)
		return ret;

	*val = temp_ext_low;

	return 0;
}

static int emc2101_temp_ext_min_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_read(data, regs_temp_ext_min, val);
}

static int emc2101_temp_ext_min_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_write(data, regs_temp_ext_min, val);
}

static int emc2101_temp_ext_type_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int beta_comp, beta_comp_auto;
	int ret;

	ret = regmap_field_read(data->fields[F_BETA_COMP], &beta_comp);
	if (ret)
		return ret;

	ret = regmap_field_read(data->fields[F_BETA_COMP], &beta_comp_auto);
	if (ret)
		return ret;

	if (beta_comp == BETA_COMP_DISABLE && !beta_comp_auto)
		*val = EMC2101_TD_2N3904;
	else
		*val = EMC2101_TD_CPU;

	return 0;
}

static int emc2101_temp_ext_type_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int beta_comp, beta_comp_auto;
	int ret;

	switch (val) {
	case EMC2101_TD_CPU:
		beta_comp = 0;
		beta_comp_auto = 1;
		break;
	case EMC2101_TD_2N3904:
		beta_comp = BETA_COMP_DISABLE;
		beta_comp_auto = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_field_write(data->fields[F_BETA_COMP_AUTO], beta_comp_auto);
	if (ret)
		return ret;

	return regmap_field_write(data->fields[F_BETA_COMP], beta_comp);
}

static int emc2101_temp_ext_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_frac_read(data, regs_temp_ext, val);
}

static int emc2101_temp_int_max_alarm_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);
	unsigned int temp_int_high;
	int ret;

	ret = regmap_field_read(data->fields[F_TEMP_INT_HIGH_ALARM], &temp_int_high);
	if (ret)
		return ret;

	*val = temp_int_high;

	return 0;
}

static int emc2101_temp_int_max_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_pos_read(data->fields[F_TEMP_INT_MAX], val);
}

static int emc2101_temp_int_max_write(struct device *dev, long val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_pos_write(data->fields[F_TEMP_INT_MAX], val);
}

static int emc2101_temp_int_input_read(struct device *dev, long *val)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return emc2101_temp_neg_read(data->fields[F_TEMP_INT], val);
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

static int emc2101_resume(struct device *dev)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return regmap_field_write(data->fields[F_STBY_MODE], 0);
}

static int emc2101_suspend(struct device *dev)
{
	struct emc2101_data *data = dev_get_drvdata(dev);

	return regmap_field_write(data->fields[F_STBY_MODE], 1);
}

static DEFINE_SIMPLE_DEV_PM_OPS(emc2101_pm, emc2101_suspend, emc2101_resume);

static int emc2101_init(struct emc2101_data *data)
{
	static const u8 lut_t[FAN_LUT_COUNT] = {  35,   40,   45,   50,   55,   60,   70,   75};
	static const u8 lut_s[FAN_LUT_COUNT] = {0x12, 0x19, 0x1f, 0x25, 0x2c, 0x32, 0x38, 0x3f};
	unsigned int i;
	u16 tach_count;
	int ret;

	/* CONFIG */
	ret = regmap_field_write(data->fields[F_PIN_FUNC_TACH], 1);
	if (ret)
		return ret;
	ret = regmap_field_write(data->fields[F_SMBUS_TOUT_DISABLE], 1);
	if (ret)
		return ret;

	/* FAN_CONFIG */
	ret = regmap_field_write(data->fields[F_TACH_FALSE_READ], TACH_FALSE_READ_DISABLE);
	if (ret)
		return ret;
	ret = regmap_field_write(data->fields[F_FAN_CLK_OVR], 1);
	if (ret)
		return ret;
	ret = regmap_field_write(data->fields[F_FAN_CLK_SEL], 0);
	if (ret)
		return ret;

	/* FAN_LUT */
	ret = regmap_field_write(data->fields[F_FAN_LUT_DISABLE], 1);
	if (ret)
		return ret;
	for (i = 0; i < FAN_LUT_COUNT; i++) {
		ret = regmap_field_write(data->fields[F_FAN_LUT_TEMP(i)], lut_t[i]);
		if (ret)
			return ret;
		ret = regmap_field_write(data->fields[F_FAN_LUT_SPEED(i)], lut_s[i]);
		if (ret)
			return ret;
	}
	ret = regmap_field_write(data->fields[F_FAN_LUT_DISABLE], 0);
	if (ret)
		return ret;

	/* PWM_FREQ */
	ret = regmap_field_write(data->fields[F_PWM_FREQ], PWM_FREQ_MASK);
	if (ret)
		return ret;
	ret = regmap_field_write(data->fields[F_PWM_FREQ_DIV], 1);
	if (ret)
		return ret;

	/* TACH gives invalid value on first reading */
	return emc2101_read_u16(data, regs_tach, &tach_count);
}

static bool emc2101_regmap_is_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case REG_TEMP_INT:	/* internal diode */
	case REG_TEMP_EXT_HI:	/* external diode high byte */
	case REG_STATUS:	/* status */
	case REG_TEMP_EXT_LO:	/* external diode low byte */
	case REG_TACH_LO:	/* tach input low byte */
	case REG_TACH_HI:	/* tach input high byte */
	case REG_FAN_SET:	/* fan pwm */
		return true;
	default:
		return false;
	}
}

static const struct regmap_config emc2101_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_MAPLE,
	.volatile_reg = emc2101_regmap_is_volatile,
};

static int emc2101_probe(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	struct device *dev = &client->dev;
	struct emc2101_data *data;
	struct device *hwmon_dev;
	unsigned int i;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	data = devm_kzalloc(dev, sizeof(struct emc2101_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = devm_regmap_init_i2c(client, &emc2101_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	for (i = 0; i < F_MAX_FIELDS; i++) {
		data->fields[i] = devm_regmap_field_alloc(dev, data->regmap,
							  emc2101_reg_fields[i]);
		if (IS_ERR(data->fields[i])) {
			dev_err(dev, "Unable to allocate regmap fields\n");
			return PTR_ERR(data->fields[i]);
		}
	}

	data->dev = dev;
	mutex_init(&data->mutex);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, data,
							 &emc2101_chip_info,
							 emc2101_hwmon_groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_dbg(dev, "%s: sensor '%s'\n", dev_name(hwmon_dev), client->name);

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

	dev_dbg(dev, "Found %s at 0x%02x (rev 0x%02x).\n", info->type, client->addr, revision);

	return 0;
}

static const struct i2c_device_id emc2101_ids[] = {
	{ "emc2101" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, emc2101_ids);

static const struct of_device_id emc2101_of_match_table[] = {
	{ .compatible = "microchip,emc2101", },
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
		.pm = pm_sleep_ptr(&emc2101_pm),
	},
	.id_table = emc2101_ids,
	.probe = emc2101_probe,
};
module_i2c_driver(emc2101_driver);

MODULE_AUTHOR("Álvaro Fernández Rojas <noltari@gmail.com>");
MODULE_DESCRIPTION("Microchip EMC2101 hwmon driver");
MODULE_LICENSE("GPL");
