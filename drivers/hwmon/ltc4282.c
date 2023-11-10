// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices LTC4282 I2C High Current Hot Swap Controller over I2C
 *
 * Copyright 2023 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/property.h>
#include <linux/units.h>

#define LTC4282_CTRL_LSB			0x00
  #define LTC4282_CTRL_OV_RETRY_MASK		BIT(0)
  #define LTC4282_CTRL_UV_RETRY_MASK		BIT(1)
  #define LTC4282_CTRL_OC_RETRY_MASK		BIT(2)
  #define LTC4282_CTRL_ON_ACTIVE_LOW_MASK	BIT(5)
  #define LTC4282_CTRL_ON_DELAY_MASK		BIT(6)
#define LTC4282_CTRL_MSB			0x01
  #define LTC4282_CTRL_VIN_MODE_MASK		GENMASK(1, 0)
  #define LTC4282_CTRL_OV_MODE_MASK		GENMASK(3, 2)
  #define LTC4282_CTRL_UV_MODE_MASK		GENMASK(5, 4)
#define LTC4282_FAULT_LOG			0x04
  #define LTC4282_OV_FAULT_MASK			BIT(0)
  #define LTC4282_UV_FAULT_MASK			BIT(1)
  #define LTC4282_OC_FAULT_MASK			BIT(2)
  #define LTC4282_POWER_BAD_FAULT_MASK		BIT(3)
  #define LTC4282_FET_SHORT_FAULT_MASK		BIT(5)
  #define LTC4282_FET_BAD_FAULT_MASK		BIT(6)
#define LTC4282_ADC_ALERT_LOG			0x05
  #define LTC4282_GPIO_ALARM_L_MASK		BIT(0)
  #define LTC4282_GPIO_ALARM_H_MASK		BIT(1)
  #define LTC4282_VSOURCE_ALARM_L_MASK		BIT(2)
  #define LTC4282_VSOURCE_ALARM_H_MASK		BIT(3)
  #define LTC4282_VSENSE_ALARM_L_MASK		BIT(4)
  #define LTC4282_VSENSE_ALARM_H_MASK		BIT(5)
  #define LTC4282_POWER_ALARM_L_MASK		BIT(6)
  #define LTC4282_POWER_ALARM_H_MASK		BIT(7)
#define LTC4282_FET_BAD_FAULT_TIMEOUT		0x06
  #define LTC4282_FET_BAD_MAX_TIMEOUT		255
#define LTC4282_GPIO_CONFIG			0x07
  #define LTC4282_GPIO_2_FET_STRESS_MASK	BIT(1)
  #define LTC4282_GPIO_1_OUT_MASK		BIT(3)
  #define LTC4282_GPIO_1_CONFIG_MASK		GENMASK(5, 4)
  #define LTC4282_GPIO_2_OUT_MASK		BIT(6)
  #define LTC4282_GPIO_3_OUT_MASK		BIT(7)
#define LTC4282_VGPIO_MIN			0x08
#define LTC4282_VGPIO_MAX			0x09
#define LTC4282_VSOURCE_MIN			0x0a
#define LTC4282_VSOURCE_MAX			0x0b
#define LTC4282_VSENSE_MIN			0x0c
#define LTC4282_VSENSE_MAX			0x0d
#define LTC4282_POWER_MIN			0x0e
#define LTC4282_POWER_MAX			0x0f
#define LTC4282_CLK_DIV				0x10
  #define LTC4282_CLK_DIV_MASK			GENMASK(4, 0)
  #define LTC4282_CLKOUT_MASK			GENMASK(6, 5)
#define LTC4282_ILIM_ADJUST			0x11
  #define LTC4282_GPIO_MODE_MASK		BIT(1)
  #define LTC4282_VDD_MONITOR_MASK		BIT(2)
  #define LTC4282_FOLDBACK_MODE_MASK		GENMASK(4, 3)
  #define LTC4282_ILIM_ADJUST_MASK		GENMASK(7, 5)
#define LTC4282_ENERGY				0x12
#define LTC4282_TIME_COUNTER			0x18
#define LTC4282_ALERT_CTRL			0x1C
  #define LTC4282_ALERT_OUT_MASK		BIT(6)
#define LTC4282_ADC_CTRL			0x1D
  #define LTC4282_FAULT_LOG_EN_MASK		BIT(2)
  #define LTC4282_METER_HALT_MASK		BIT(5)
  #define LTC4282_METER_RESET_MASK		BIT(6)
  #define LTC4282_RESET_MASK			BIT(7)
#define LTC4282_STATUS_LSB			0x1E
  #define LTC4282_OV_STATUS_MASK		BIT(0)
  #define LTC4282_UV_STATUS_MASK		BIT(1)
  #define LTC4282_VDD_STATUS_MASK		(LTC4282_OV_STATUS_MASK | LTC4282_UV_STATUS_MASK)
  #define LTC4282_OC_STATUS_MASK		BIT(2)
  #define LTC4282_POWER_GOOD_MASK		BIT(3)
  #define LTC4282_FET_SHORT_MASK		BIT(5)
  #define LTC4282_FET_BAD_STATUS_MASK		BIT(6)
#define LTC4282_STATUS_MSB			0x1F
  #define LTC4282_METER_TICK_OVERFLOW_MASK	GENMASK(1, 0)
  #define LTC4282_ALERT_STATUS_MASK		BIT(4)
  #define LTC4282_GPIO_1_STATUS_MASK		BIT(5)
  #define LTC4282_GPIO_2_STATUS_MASK		BIT(6)
  #define LTC4282_GPIO_3_STATUS_MASK		BIT(7)
#define LTC4282_RESERVED_1			0x32
#define LTC4282_RESERVED_2			0x33
#define LTC4282_VGPIO				0x34
#define LTC4282_VGPIO_LOWEST			0x36
#define LTC4282_VGPIO_HIGHEST			0x38
#define LTC4282_VSOURCE				0x3a
#define LTC4282_VSOURCE_LOWEST			0x3c
#define LTC4282_VSOURCE_HIGHEST			0x3e
#define LTC4282_VSENSE				0x40
#define LTC4282_VSENSE_LOWEST			0x42
#define LTC4282_VSENSE_HIGHEST			0x44
#define LTC4282_POWER				0x46
#define LTC4282_POWER_LOWEST			0x48
#define LTC4282_POWER_HIGHEST			0x4a
#define LTC4282_RESERVED_3			0x50

#define LTC4282_CLKIN_MIN	(250 * KILO)
#define LTC4282_CLKIN_MAX	(15500 * KILO)
/* this assumes 12bit ADC */
#define LTC4282_TCONV_US	65535
#define LTC4282_GPIO_NR		4
/*
 * relaxed version of FIELD_PREP() to be used when mask is not a compile time constant
 * u32_encode_bits() can't also be used as the compiler needs to be able to evaluate
 * mask at compile time.
 */
#define LTC4282_FIELD_PREP(m, v)	(((v) << (ffs(m) - 1)) & (m))

struct ltc4282_state {
	struct regmap *map;
	struct device *dev;
	/* Protect against multiple accesses to the device registers */
	struct mutex lock;
	struct gpio_chip gc;
	u64 saved_energy;
	long power_max;
	u32 gpio_map[LTC4282_GPIO_NR];
	u32 rsense;
	u32 vin_mode;
	u16 vfs_out;
};

struct ltc4282_gpio {
	u32 out_reg;
	u32 out_mask;
	u32 in_reg;
	u32 in_mask;
	bool active_high;
	u8 n_funcs;
};

enum {
	LTC4282_VIN_3_3V,
	LTC4282_VIN_5V,
	LTC4282_VIN_12V,
	LTC4282_VIN_24V,
};

enum {
	LTC4282_CHAN_VSOURCE,
	LTC4282_CHAN_VDD,
	LTC4282_CHAN_VGPIO,
};

enum {
	LTC4282_GPIO_1,
	LTC4282_GPIO_2,
	LTC4282_GPIO_3,
	LTC4282_ALERT,
};

static const struct ltc4282_gpio ltc4282_gpios[] = {
	[LTC4282_GPIO_1] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_1_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_1_OUT_MASK,
		.active_high = true,
		.n_funcs = 3,
	},
	[LTC4282_GPIO_2] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_2_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_2_OUT_MASK,
		.n_funcs = 3,
	},
	[LTC4282_GPIO_3] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_3_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_3_OUT_MASK,
		.n_funcs = 2,
	},
	[LTC4282_ALERT] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_ALERT_STATUS_MASK,
		.out_reg = LTC4282_ALERT_CTRL,
		.out_mask = LTC4282_ALERT_OUT_MASK,
	},
};

static int ltc4282_gpio_input_set(struct gpio_chip *chip, unsigned int offset)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	u32 gpio_pin = st->gpio_map[offset];

	/* we can only control this for GPIO_1 */
	if (gpio_pin != LTC4282_GPIO_1)
		return 0;

	return regmap_set_bits(st->map, LTC4282_GPIO_CONFIG, LTC4282_GPIO_1_CONFIG_MASK);
}

static int ltc4282_gpio_output_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	u32 gpio_pin = st->gpio_map[offset];
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[gpio_pin];

	guard(mutex)(&st->lock);
	/*
	 * Explicitly setting the pin as output can only be done for GPIO_1. For the
	 * other pins we just pull the line down or high-z.
	 */
	if (gpio_pin == LTC4282_GPIO_1) {
		int ret;

		ret = regmap_update_bits(st->map, LTC4282_GPIO_CONFIG, LTC4282_GPIO_1_CONFIG_MASK,
					 FIELD_PREP(LTC4282_GPIO_1_CONFIG_MASK, 2));
		if (ret)
			return ret;
	}

	/* GPIO_2,3 and the ALERT pin require setting the bit to 1 to pull down the line */
	if (!gpio->active_high)
		val = !val;

	return regmap_update_bits(st->map, gpio->out_reg, gpio->out_mask,
				  LTC4282_FIELD_PREP(gpio->out_mask, val));
}

static void ltc4282_gpio_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	u32 gpio_pin = st->gpio_map[offset];
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[gpio_pin];

	if (!gpio->active_high)
		val = !val;

	regmap_update_bits(st->map, gpio->out_reg, gpio->out_mask,
			   LTC4282_FIELD_PREP(gpio->out_mask, val));
}

static int ltc4282_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	u32 gpio_pin = st->gpio_map[offset];
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[gpio_pin];
	int ret;
	u32 val;

	ret = regmap_read(st->map, gpio->in_reg, &val);
	if (ret)
		return ret;

	return !!(val & gpio->in_mask);
}

static int ltc4282_read_voltage_word(const struct ltc4282_state *st, u32 reg, u32 fs, long *val)
{
	__be16 in;
	int ret;

	ret = regmap_bulk_read(st->map, reg, &in, sizeof(in));
	if (ret)
		return ret;

	/*
	 * This is also used to calculate current in which case fs comes in 10 * uV.
	 * Hence the ULL usage.
	 */
	*val = DIV_ROUND_CLOSEST_ULL(be16_to_cpu(in) * (u64)fs, U16_MAX);
	return 0;
}

static int ltc4282_read_voltage_byte(const struct ltc4282_state *st, u32 reg, u32 fs, long *val)
{
	int ret;
	u32 in;

	ret = regmap_read(st->map, reg, &in);
	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(in * fs, U8_MAX);
	return 0;
}

static int ltc4282_read_vdd(struct ltc4282_state *st, u32 fs, long *val)
{
	int ret;

	guard(mutex)(&st->lock);

	/* ADC now monitors VDD */
	ret = regmap_clear_bits(st->map, LTC4282_ILIM_ADJUST, LTC4282_VDD_MONITOR_MASK);
	if (ret)
		return ret;

	/*
	 * Wait for two ADC conversions so we are sure we get one full VDD
	 * measurement.
	 */
	msleep(2 * LTC4282_TCONV_US / MILLI);

	ret = ltc4282_read_voltage_word(st, LTC4282_VSOURCE, st->vfs_out, val);
	if (ret)
		return ret;

	/* back to VSOURCE */
	return regmap_set_bits(st->map, LTC4282_ILIM_ADJUST, LTC4282_VDD_MONITOR_MASK);
}

static int ltc4282_read_alarm(struct ltc4282_state *st, u32 reg, u32 mask, long *val)
{
	u32 alarm;
	int ret;

	guard(mutex)(&st->lock);

	/* if not status, clear first the alarm by clearing the bit */
	if (reg != LTC4282_STATUS_LSB && reg != LTC4282_FAULT_LOG) {
		ret = regmap_clear_bits(st->map, reg, mask);
		if (ret)
			return ret;

		msleep(LTC4282_TCONV_US / MILLI);
	}

	ret = regmap_read(st->map, reg, &alarm);
	if (ret)
		return ret;

	*val = !!(alarm & mask);

	return 0;
}

static int ltc4282_read_in(struct device *dev, u32 attr, long *val, u32 channel)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_in_input:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_voltage_word(st, LTC4282_VSOURCE, st->vfs_out, val);
		if (channel == LTC4282_CHAN_VDD)
			return ltc4282_read_vdd(st, st->vfs_out, val);

		return ltc4282_read_voltage_word(st, LTC4282_VGPIO, 1280, val);
	case hwmon_in_highest:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_voltage_word(st, LTC4282_VSOURCE_HIGHEST,
							 st->vfs_out, val);

		return ltc4282_read_voltage_word(st, LTC4282_VGPIO_HIGHEST, 1280, val);
	case hwmon_in_lowest:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_voltage_word(st, LTC4282_VSOURCE_LOWEST,
							 st->vfs_out, val);

		return ltc4282_read_voltage_word(st, LTC4282_VGPIO_LOWEST, 1280, val);
	case hwmon_in_max_alarm:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
						  LTC4282_VSOURCE_ALARM_H_MASK, val);

		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_GPIO_ALARM_H_MASK,
					  val);
	case hwmon_in_min_alarm:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
						  LTC4282_VSOURCE_ALARM_L_MASK, val);

		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_GPIO_ALARM_L_MASK,
					  val);
	case hwmon_in_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB,
					  LTC4282_FET_BAD_STATUS_MASK, val);
	case hwmon_in_crit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB, LTC4282_OV_STATUS_MASK, val);
	case hwmon_in_lcrit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB, LTC4282_UV_STATUS_MASK, val);
	case hwmon_in_max:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_voltage_byte(st, LTC4282_VSOURCE_MAX,
							 st->vfs_out, val);

		return ltc4282_read_voltage_byte(st, LTC4282_VGPIO_MAX, 1280, val);
	case hwmon_in_min:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_read_voltage_byte(st, LTC4282_VSOURCE_MIN,
							 st->vfs_out, val);

		return ltc4282_read_voltage_byte(st, LTC4282_VGPIO_MIN, 1280, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_read_current_word(const struct ltc4282_state *st, u32 reg, long *val)
{
	long in;
	int ret;

	/*
	 * We pass in full scale in 10 * micro (note that 40 is already millivolt) so we
	 * have better approximations to calculate current.
	 */
	ret = ltc4282_read_voltage_word(st, reg, DECA * 40 * MILLI, &in);
	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(in * MILLI, st->rsense);

	return 0;
}

static int ltc4282_read_current_byte(const struct ltc4282_state *st, u32 reg, long *val)
{
	long in;
	int ret;

	ret = ltc4282_read_voltage_byte(st, reg, DECA * 40 * MILLI, &in);
	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(in * MILLI, st->rsense);

	return 0;
}

static int ltc4282_read_curr(struct device *dev, const u32 attr, long *val)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_curr_input:
		return ltc4282_read_current_word(st, LTC4282_VSENSE, val);
	case hwmon_curr_highest:
		return ltc4282_read_current_word(st, LTC4282_VSENSE_HIGHEST, val);
	case hwmon_curr_lowest:
		return ltc4282_read_current_word(st, LTC4282_VSENSE_LOWEST, val);
	case hwmon_curr_max:
		return ltc4282_read_current_byte(st, LTC4282_VSENSE_MAX, val);
	case hwmon_curr_min:
		return ltc4282_read_current_byte(st, LTC4282_VSENSE_MIN, val);
	case hwmon_curr_max_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_VSENSE_ALARM_H_MASK,
					  val);
	case hwmon_curr_min_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_VSENSE_ALARM_L_MASK,
					  val);
	case hwmon_curr_crit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB, LTC4282_OC_STATUS_MASK, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_read_power_word(const struct ltc4282_state *st, u32 reg, long *val)
{
	u64 temp =  DECA * 40ULL * st->vfs_out * 1 << 16, temp_2;
	__be16 raw;
	u16 power;
	int ret;

	ret = regmap_bulk_read(st->map, reg, &raw, sizeof(raw));
	if (ret)
		return ret;

	power = be16_to_cpu(raw);
	/*
	 * Power is given by:
	 *	P = CODE(16b) * 0.040 * Vfs(out) * 2^16 / ((2^16 - 1)^2 * Rsense)
	 */
	if (check_mul_overflow(power * temp, MICRO, &temp_2)) {
		temp = DIV_ROUND_CLOSEST_ULL(power * temp, U16_MAX);
		*val = DIV64_U64_ROUND_CLOSEST(temp * MICRO, U16_MAX * (u64)st->rsense);
		return 0;
	}

	*val = DIV64_U64_ROUND_CLOSEST(temp_2, st->rsense * int_pow(U16_MAX, 2));

	return 0;
}

static int ltc4282_read_power_byte(const struct ltc4282_state *st, u32 reg, long *val)
{
	u32 power;
	u64 temp;
	int ret;

	ret = regmap_read(st->map, reg, &power);
	if (ret)
		return ret;

	temp = power * 40 * DECA * st->vfs_out * 256ULL;
	*val = DIV64_U64_ROUND_CLOSEST(temp * MICRO, int_pow(U8_MAX, 2) * st->rsense);

	return 0;
}

static int ltc4282_read_energy(const struct ltc4282_state *st, u64 *val)
{
	u64 temp, energy;
	u32 status;
	__be64 raw;
	int ret;

	ret = regmap_bulk_read(st->map, LTC4282_ENERGY, &raw, 6);
	if (ret)
		return ret;

	ret = regmap_read(st->map, LTC4282_STATUS_MSB, &status);
	if (ret)
		return ret;

	if (status & LTC4282_METER_TICK_OVERFLOW_MASK) {
		dev_warn(st->dev, "Got overflow in meter/ticker counters\n");
		/*
		 * This resets the meter and the tick counter and holds them
		 * reset.
		 */
		ret = regmap_set_bits(st->map, LTC4282_ADC_CTRL,
				      LTC4282_METER_RESET_MASK);
		if (ret)
			return ret;

		/* start accumulating again */
		ret = regmap_clear_bits(st->map, LTC4282_ADC_CTRL,
					LTC4282_METER_RESET_MASK);
		if (ret)
			return ret;

		/*
		 * Let the callers know a reset happened. Important when calling
		 * from power1_average.
		 */
		ret = 1;
	}

	energy =  be64_to_cpu(raw) >> 16;
	/*
	 * The formula for energy is given by:
	 *	E = CODE(48b) * 0.040 * Vfs(out) * Tconv * 256 / ((2^16 - 1)^2 * Rsense)
	 *
	 * Since we only support 12bit ADC, Tconv = 0.065535s. Passing Vfs(out) and 0.040 to
	 * mV and Tconv to us, we can simplify the formula to:
	 *	E = CODE(48b) * 40 * Vfs(out) * 256 / (U16_MAX * Rsense)
	 *
	 * As Rsense is in tens of micro-ohm, we need to multiply by DECA to get
	 * microujoule.
	 */
	if (check_mul_overflow(DECA * st->vfs_out * 40 * 256, energy, &temp)) {
		temp = DIV_ROUND_CLOSEST(DECA * st->vfs_out * 40 * 256, U16_MAX);
		*val = DIV_ROUND_CLOSEST_ULL(temp * energy, st->rsense);
		return ret;
	}

	*val = DIV64_U64_ROUND_CLOSEST(temp, U16_MAX * (u64)st->rsense);

	return ret;
}

static int ltc4282_read_power_average(struct ltc4282_state *st, long *val)
{
	u64 energy, temp;
	__be32 raw;
	u32 count;
	int ret;

	guard(mutex)(&st->lock);

	ret = ltc4282_read_energy(st, &energy);
	if (ret < 0)
		return ret;
	if (ret == 1) {
		/*
		 * reset happened... let's read the new energy value that
		 * together with the new tick counter should give a sane average
		 * value. Furthermore, we save whatever value we had accumulated
		 * so that the next energy read will have it into account.
		 */
		st->saved_energy = energy;
		/* give some time for accumulation... */
		msleep(2 * LTC4282_TCONV_US / MILLI);
		ret = ltc4282_read_energy(st, &energy);
		if (ret < 0)
			return ret;
	}

	ret = regmap_bulk_read(st->map, LTC4282_TIME_COUNTER, &raw, sizeof(raw));
	if (ret)
		return ret;

	count = be32_to_cpu(raw);
	if (!count) {
		*val = 0;
		return 0;
	}

	/*
	 * AVG = E / (Tconv * counter)
	 * We get energy in microJoule, hence dividing it by microSeconds gives Watts. Therefore,
	 * multiplying by MICRO gives us microWatts.
	 */
	if (check_mul_overflow(energy, MICRO, &temp)) {
		temp = DIV_ROUND_CLOSEST_ULL(energy, LTC4282_TCONV_US);
		*val = DIV_ROUND_CLOSEST_ULL(temp * MICRO, count);
		return 0;
	}

	*val = DIV64_U64_ROUND_CLOSEST(temp, LTC4282_TCONV_US * (u64)count);
	return 0;
}

static int ltc4282_read_power(struct device *dev, const u32 attr, long *val)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_power_input:
		return ltc4282_read_power_word(st, LTC4282_POWER, val);
	case hwmon_power_input_highest:
		return ltc4282_read_power_word(st, LTC4282_POWER_HIGHEST, val);
	case hwmon_power_input_lowest:
		return ltc4282_read_power_word(st, LTC4282_POWER_LOWEST, val);
	case hwmon_power_max_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_POWER_ALARM_H_MASK,
					  val);
	case hwmon_power_min_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, LTC4282_POWER_ALARM_L_MASK,
					  val);
	case hwmon_power_average:
		return ltc4282_read_power_average(st, val);
	case hwmon_power_max:
		return ltc4282_read_power_byte(st, LTC4282_POWER_MAX, val);
	case hwmon_power_min:
		return ltc4282_read_power_byte(st, LTC4282_POWER_MIN, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_energy_enable(struct device *dev, long *val)
{
	const struct ltc4282_state *st = dev_get_drvdata(dev);
	u32 reg_val;
	int ret;

	ret = regmap_read(st->map, LTC4282_ADC_CTRL, &reg_val);
	if (ret)
		return ret;

	*val = !FIELD_GET(LTC4282_METER_HALT_MASK, reg_val);

	return 0;
}

static int ltc4282_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_in:
		return ltc4282_read_in(dev, attr, val, channel);
	case hwmon_curr:
		return ltc4282_read_curr(dev, attr, val);
	case hwmon_power:
		return ltc4282_read_power(dev, attr, val);
	case hwmon_energy:
		return ltc4282_energy_enable(dev, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_write_power_byte(const struct ltc4282_state *st, u32 reg, long val)
{
	u32 power;
	u64 temp;

	if (val > st->power_max)
		val = st->power_max;

	temp = val * int_pow(U8_MAX, 2) * st->rsense;
	power = DIV64_U64_ROUND_CLOSEST(temp, MICRO * DECA * 256ULL * st->vfs_out * 40);

	return regmap_write(st->map, reg, power);
}

static int ltc4282_write_power(const struct ltc4282_state *st, u32 attr,
			       long val)
{
	switch (attr) {
	case hwmon_power_max:
		return ltc4282_write_power_byte(st, LTC4282_POWER_MAX, val);
	case hwmon_power_min:
		return ltc4282_write_power_byte(st, LTC4282_POWER_MIN, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_write_voltage_byte(const struct ltc4282_state *st, u32 reg, u32 fs, long val)
{
	u32 in;

	if (val >= fs)
		in = U8_MAX;
	else
		in = DIV_ROUND_CLOSEST(val * U8_MAX, fs);

	return regmap_write(st->map, reg, in);
}

static int ltc4282_write_in(const struct ltc4282_state *st, u32 attr, long val,
			    int channel)
{
	switch (attr) {
	case hwmon_in_max:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_write_voltage_byte(st, LTC4282_VSOURCE_MAX,
							  st->vfs_out, val);

		return ltc4282_write_voltage_byte(st, LTC4282_VGPIO_MAX, 1280, val);
	case hwmon_in_min:
		if (channel == LTC4282_CHAN_VSOURCE)
			return ltc4282_write_voltage_byte(st, LTC4282_VSOURCE_MIN,
							  st->vfs_out, val);

		return ltc4282_write_voltage_byte(st, LTC4282_VGPIO_MIN, 1280, val);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_write_curr(const struct ltc4282_state *st, u32 attr,
			      long val)
{
	/* need to pass it in millivolt */
	u32 in = DIV_ROUND_CLOSEST_ULL((u64)val * st->rsense, DECA * MICRO);

	switch (attr) {
	case hwmon_curr_max:
		return ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MAX, 40, in);
	case hwmon_curr_min:
		return ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MIN, 40, in);
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_write(struct device *dev,
			 enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	const struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_power:
		return ltc4282_write_power(st, attr, val);
	case hwmon_in:
		return ltc4282_write_in(st, attr, val, channel);
	case hwmon_curr:
		return ltc4282_write_curr(st, attr, val);
	case hwmon_energy:
		/* setting the bit halts the meter */
		return regmap_update_bits(st->map, LTC4282_ADC_CTRL,
					  LTC4282_METER_HALT_MASK,
					  FIELD_PREP(LTC4282_METER_HALT_MASK, !!!val));
	default:
		return -ENOTSUPP;
	}
}

static int ltc4282_in_is_visible(const struct ltc4282_state *st, u32 attr)
{
	switch (attr) {
	case hwmon_in_input:
	case hwmon_in_highest:
	case hwmon_in_lowest:
	case hwmon_in_max_alarm:
	case hwmon_in_min_alarm:
	case hwmon_in_label:
	case hwmon_in_lcrit_alarm:
	case hwmon_in_crit_alarm:
		return 0444;
	case hwmon_in_max:
	case hwmon_in_min:
		return 0644;
	default:
		return 0;
	}
}

static int ltc4282_curr_is_visible(u32 attr)
{
	switch (attr) {
	case hwmon_curr_input:
	case hwmon_curr_highest:
	case hwmon_curr_lowest:
	case hwmon_curr_max_alarm:
	case hwmon_curr_min_alarm:
	case hwmon_curr_crit_alarm:
	case hwmon_curr_label:
		return 0444;
	case hwmon_curr_max:
	case hwmon_curr_min:
		return 0644;
	default:
		return 0;
	}
}

static int ltc4282_power_is_visible(u32 attr)
{
	switch (attr) {
	case hwmon_power_input:
	case hwmon_power_input_highest:
	case hwmon_power_input_lowest:
	case hwmon_power_label:
	case hwmon_power_max_alarm:
	case hwmon_power_min_alarm:
	case hwmon_power_average:
		return 0444;
	case hwmon_power_max:
	case hwmon_power_min:
		return 0644;
	default:
		return 0;
	}
}

static umode_t ltc4282_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		return ltc4282_in_is_visible(data, attr);
	case hwmon_curr:
		return ltc4282_curr_is_visible(attr);
	case hwmon_power:
		return ltc4282_power_is_visible(attr);
	case hwmon_energy:
		/* hwmon_energy_enable */
		return 0644;
	default:
		return -ENOTSUPP;
	}
}

static const char * const ltc4282_in_strs[] = {
	"VSOURCE", "VDD", "VGPIO"
};

static int ltc4282_read_labels(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_in:
		*str = ltc4282_in_strs[channel];
		return 0;
	case hwmon_curr:
		*str = "ISENSE";
		return 0;
	case hwmon_power:
		*str = "Power";
		return 0;
	default:
		return -ENOTSUPP;
	}
}

static ssize_t ltc4282_show_energy(struct device *dev,
				   struct device_attribute *da, char *buf)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	u64 energy;
	long en;
	int ret;

	guard(mutex)(&st->lock);

	ret = ltc4282_energy_enable(dev, &en);
	if (ret)
		return ret;
	if (!en)
		return -ENODATA;

	ret = ltc4282_read_energy(st, &energy);
	if (ret < 0)
		return ret;

	/* see @ltc4282_read_power_average */
	if (st->saved_energy) {
		energy += st->saved_energy;
		st->saved_energy = 0;
	}

	return sysfs_emit(buf, "%llu\n", energy);
}

static ssize_t ltc4282_show_fault(struct device *dev,
				  struct device_attribute *da,
				  u32 reg, char *buf)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	long alarm;
	int ret;

	ret = ltc4282_read_alarm(st, reg, attr->index, &alarm);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", alarm);
}

static ssize_t ltc4282_show_status(struct device *dev,
				   struct device_attribute *da, char *buf)
{
	return ltc4282_show_fault(dev, da, LTC4282_STATUS_LSB, buf);
}

static ssize_t ltc4282_show_fault_log(struct device *dev,
				      struct device_attribute *da, char *buf)
{
	return ltc4282_show_fault(dev, da, LTC4282_FAULT_LOG, buf);
}

static ssize_t ltc4282_clear_fault_log(struct device *dev,
				       struct device_attribute *da,
				       const char *buf, size_t len)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int ret;

	ret = regmap_clear_bits(st->map, LTC4282_FAULT_LOG, attr->index);
	if (ret)
		return ret;

	return len;
}

static int ltc4282_power_on(const struct ltc4282_state *st)
{
	u32 n_tries = 5, reg;
	int ret;

	ret = devm_regulator_get_enable(st->dev, "vdd");
	if (ret)
		return dev_err_probe(st->dev, ret, "Failed vdd get/enable\n");

	/*
	 * Make sure vdd is stable. From the datasheet:
	 * The state of the UV and OV comparators is indicated by the STATUS register
	 * bits 0 and 1 and must be stable for at least 50ms to qualify for turn-on.
	 */
	do {
		ret = regmap_read_poll_timeout(st->map, LTC4282_STATUS_LSB, reg,
					       reg & LTC4282_VDD_STATUS_MASK, 10000, 50000);
		if (!ret)
			continue;
		if (ret != -ETIMEDOUT)
			return dev_err_probe(st->dev, ret, "Failed regmap read\n");

		/* Alright. We got timeout which means UV and OV are stable for 50ms */
		break;
	} while (n_tries--);

	if (!n_tries)
		return dev_err_probe(st->dev, -ETIMEDOUT, "VDD not stable\n");

	return 0;
}

enum {
	LTC4282_CLKOUT_INT,
	LTC4282_CLKOUT_TICK,
};

static int ltc428_clks_setup(const struct ltc4282_state *st)
{
	unsigned long rate;
	struct clk *clkin;
	u32 val;
	int ret;

	ret = device_property_read_u32(st->dev, "adi,clkout-mode", &val);
	if (ret)
		return 0;

	if (val > LTC4282_CLKOUT_TICK)
		return dev_err_probe(st->dev, -EINVAL,
				     "Invalid val(%u) for adi,clkout-mode\n", val);

	ret = regmap_update_bits(st->map, LTC4282_CLK_DIV, LTC4282_CLKOUT_MASK,
				 FIELD_PREP(LTC4282_CLKOUT_MASK, val + 1));
	if (ret)
		return ret;

	clkin = devm_clk_get_optional_enabled(st->dev, NULL);
	if (IS_ERR(clkin))
		return dev_err_probe(st->dev, PTR_ERR(clkin), "Failed to get clkin");
	if (!clkin)
		return 0;

	rate = clk_get_rate(clkin);
	if (rate < LTC4282_CLKIN_MIN || rate > LTC4282_CLKIN_MAX)
		return dev_err_probe(st->dev, -EINVAL, "Invalid clkin range(%lu) [%lu %lu]\n",
				     rate, LTC4282_CLKIN_MIN, LTC4282_CLKIN_MAX);

	/*
	 * Clocks faster than 250KHZ should be reduced to 250KHZ. The clock frequency
	 * is divided by twice the value in the register.
	 */
	val = rate / (2 * LTC4282_CLKIN_MIN);

	return regmap_update_bits(st->map, LTC4282_CLK_DIV, LTC4282_CLK_DIV_MASK,
				  FIELD_PREP(LTC4282_CLK_DIV_MASK, val));
}

static const int ltc4282_curr_lim_uv[] = {
	12500, 15625, 18750, 21875, 25000, 28125, 31250, 34375
};

static int ltc4282_get_defaults(struct ltc4282_state *st, u32 *curr_lim_uv)
{
	u32 reg_val, ilm_adjust;
	int ret;

	ret = regmap_read(st->map, LTC4282_CTRL_MSB, &reg_val);
	if (ret)
		return ret;

	st->vin_mode = FIELD_GET(LTC4282_CTRL_VIN_MODE_MASK, reg_val);

	ret = regmap_read(st->map, LTC4282_ILIM_ADJUST, &reg_val);
	if (ret)
		return ret;

	ilm_adjust = FIELD_GET(LTC4282_ILIM_ADJUST_MASK, reg_val);
	*curr_lim_uv = ltc4282_curr_lim_uv[ilm_adjust];

	return 0;
}

/*
 * Set max limits for ISENSE and Power as that depends on the max voltage on rsense
 * that is defined in ILIM_ADJUST. This is specially important for power because for
 * some rsense and vfsout values, if we allow the default raw 255 value, that would
 * overflow long in 32bit archs when reading back the max power limit.
 */
static int ltc4282_set_max_limits(struct ltc4282_state *st, u32 val_uv)
{
	int ret;

	ret = ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MAX, 40 * MILLI, val_uv);
	if (ret)
		return ret;

	/* Power is given by ISENSE * Vout. */
	st->power_max = DIV_ROUND_CLOSEST(val_uv * DECA * MILLI, st->rsense) * st->vfs_out;
	return ltc4282_write_power_byte(st, LTC4282_POWER_MAX, st->power_max);
}

/* valid GPIO functions */
enum {
	LTC4282_PIN_GPIO,
	/* Power functions only for GPIO_1*/
	LTC4282_PIN_POWER_BAD,
	LTC4282_PIN_POWER_GOOD,
	/* ADC monitor only for GPIO_2 and 3 */
	LTC4282_PIN_ADC = 2,
	/* Only for GPIO_2 */
	LTC4282_PIN_FET_STRESS,
};

static int ltc4282_non_gpio_setup(struct ltc4282_state *st, u32 pin, u32 func, bool *adc_in)
{
	if (pin == LTC4282_GPIO_1) {
		u32 val = LTC4282_PIN_POWER_BAD;

		if (func == LTC4282_PIN_POWER_GOOD)
			val = 0;

		return regmap_update_bits(st->map, LTC4282_GPIO_CONFIG, LTC4282_GPIO_1_CONFIG_MASK,
					  FIELD_PREP(LTC4282_GPIO_1_CONFIG_MASK, val));
	}

	if (func == LTC4282_PIN_FET_STRESS)
		return regmap_update_bits(st->map, LTC4282_GPIO_CONFIG,
					  LTC4282_GPIO_2_FET_STRESS_MASK,
					  FIELD_PREP(LTC4282_GPIO_2_FET_STRESS_MASK, 1));

	/*
	 * Then, let's point the given GPIO to the ADC input. We need to ensure that
	 * function is only given once.
	 */
	if (*adc_in)
		return dev_err_probe(st->dev, -EINVAL,
				     "Only one gpio can be given to the ADC input\n");

	*adc_in = true;

	/* setting the bit to 1 cause the ADC to monitor GPIO2 */
	return regmap_update_bits(st->map, LTC4282_ILIM_ADJUST, LTC4282_GPIO_MODE_MASK,
				  FIELD_PREP(LTC4282_GPIO_MODE_MASK, pin == LTC4282_GPIO_2));
}

static const char * const ltc4282_gpio_prop[] = {
	"adi,gpio1-mode", "adi,gpio2-mode", "adi,gpio3-mode"
};

static int ltc4282_gpio_setup(struct ltc4282_state *st)
{
	struct device *dev = st->dev;
	u32 gpio, func, ngpios = 0;
	bool adc_in = false;
	int ret;

	if (!IS_ENABLED(CONFIG_GPIOLIB))
		return 0;

	for (gpio = 0; gpio <= LTC4282_GPIO_3; gpio++) {
		ret = device_property_read_u32(dev, ltc4282_gpio_prop[gpio], &func);
		if (ret)
			continue;
		if (func >= ltc4282_gpios[gpio].n_funcs)
			return dev_err_probe(dev, ret, "Invalid func(%u >= %u) for gpio%u\n",
					     func, ltc4282_gpios[gpio].n_funcs, gpio + 1);
		if (func == LTC4282_PIN_GPIO) {
			st->gpio_map[ngpios++] = gpio;
			if (gpio == LTC4282_GPIO_1) {
				/* default to input GPIO */
				ret = regmap_set_bits(st->map, LTC4282_GPIO_CONFIG,
						      LTC4282_GPIO_1_CONFIG_MASK);
				if (ret)
					return ret;
			}

			continue;
		}

		ret = ltc4282_non_gpio_setup(st, gpio, func, &adc_in);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,gpio-alert"))
		st->gpio_map[ngpios++] = LTC4282_ALERT;

	if (!ngpios)
		return 0;

	st->gc.parent = dev;
	st->gc.base = -1;
	st->gc.ngpio = ngpios;
	st->gc.can_sleep = true;
	st->gc.label = "ltc4282";
	st->gc.direction_input = ltc4282_gpio_input_set;
	st->gc.direction_output = ltc4282_gpio_output_set;
	st->gc.set = ltc4282_gpio_set;
	st->gc.get = ltc4282_gpio_get;

	return devm_gpiochip_add_data(dev, &st->gc, st);
}

/* This maps the Vout full scale for the given Vin mode */
static const u16 ltc4282_vfs_milli[] = { 5540, 8320, 16640, 33280 };

enum {
	LTC4282_DIV_EXTERNAL,
	LTC4282_DIV_5_PERCENT,
	LTC4282_DIV_10_PERCENT,
	LTC4282_DIV_15_PERCENT,
};

static int ltc4282_setup(struct ltc4282_state *st)
{
	struct device *dev = st->dev;
	u32 val, curr_lim_uv;
	int ret;

	/* The part has an eeprom so let's get the needed defaults from it */
	ret = ltc4282_get_defaults(st, &curr_lim_uv);
	if (ret)
		return ret;

	ret = device_property_read_u32(dev, "adi,rsense-nano-ohms", &st->rsense);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read adi,rsense-nano-ohms\n");

	/*
	 * The resolution for rsense is tens of micro which means we need nano in the bindings.
	 * However, to make things easier to handle (with respect to overflows) we divide it by
	 * 100 as we don't really need the last two digits.
	 */
	st->rsense /= CENTI;

	ret = device_property_read_u32(dev, "vin-mode-microvolt", &val);
	if (!ret) {
		switch (val) {
		case 3300000:
			st->vin_mode = LTC4282_VIN_3_3V;
			break;
		case 5000000:
			st->vin_mode = LTC4282_VIN_5V;
			break;
		case 12000000:
			st->vin_mode = LTC4282_VIN_12V;
			break;
		case 24000000:
			st->vin_mode = LTC4282_VIN_24V;
			break;
		default:
			return dev_err_probe(dev, -EINVAL,
					     "Invalid val(%u) for vin-mode-microvolt\n", val);
		}

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB,  LTC4282_CTRL_VIN_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_VIN_MODE_MASK, st->vin_mode));
		if (ret)
			return ret;

		/* Foldback mode should also be set to the input voltage */
		ret = regmap_update_bits(st->map, LTC4282_ILIM_ADJUST, LTC4282_FOLDBACK_MODE_MASK,
					 FIELD_PREP(LTC4282_FOLDBACK_MODE_MASK, st->vin_mode));
		if (ret)
			return ret;
	}

	st->vfs_out = ltc4282_vfs_milli[st->vin_mode];

	ret = device_property_read_u32(dev, "adi,current-limit-microvolt",
				       &curr_lim_uv);
	if (!ret) {
		int reg_val;

		switch (val) {
		case 12500:
			reg_val = 0;
			break;
		case 15625:
			reg_val = 1;
			break;
		case 18750:
			reg_val = 2;
			break;
		case 21875:
			reg_val = 3;
			break;
		case 25000:
			reg_val = 4;
			break;
		case 28125:
			reg_val = 5;
			break;
		case 31250:
			reg_val = 6;
			break;
		case 34375:
			reg_val = 7;
			break;
		default:
			return dev_err_probe(dev, -EINVAL,
					     "Invalid val(%u) for adi,current-limit-microvolt\n",
					     val);
		}

		ret = regmap_update_bits(st->map, LTC4282_ILIM_ADJUST, LTC4282_ILIM_ADJUST_MASK,
					 FIELD_PREP(LTC4282_ILIM_ADJUST_MASK, reg_val));
		if (ret)
			return ret;
	}

	ret = ltc4282_set_max_limits(st, curr_lim_uv);
	if (ret)
		return ret;

	ret = device_property_read_u32(st->dev, "adi,overvoltage-dividers", &val);
	if (!ret) {
		if (val > LTC4282_DIV_15_PERCENT)
			return dev_err_probe(st->dev, -EINVAL,
					     "Invalid val(%u) for adi,overvoltage-divider\n", val);

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB, LTC4282_CTRL_OV_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_OV_MODE_MASK, val));
	}

	ret = device_property_read_u32(st->dev, "adi,undervoltage-dividers", &val);
	if (!ret) {
		if (val > LTC4282_DIV_15_PERCENT)
			return dev_err_probe(st->dev, -EINVAL,
					     "Invalid val(%u) for adi,undervoltage-divider\n", val);

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB, LTC4282_CTRL_UV_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_UV_MODE_MASK, val));
	}

	if (device_property_read_bool(dev, "adi,overcurrent-retry")) {
		ret = regmap_set_bits(st->map, LTC4282_CTRL_LSB, LTC4282_CTRL_OC_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,overvoltage-retry-disable")) {
		ret = regmap_clear_bits(st->map, LTC4282_CTRL_LSB, LTC4282_CTRL_OV_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,undervoltage-retry-disable")) {
		ret = regmap_clear_bits(st->map, LTC4282_CTRL_LSB, LTC4282_CTRL_UV_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,fault-log-enable")) {
		ret = regmap_set_bits(st->map, LTC4282_ADC_CTRL, LTC4282_FAULT_LOG_EN_MASK);
		if (ret)
			return ret;
	}

	ret = device_property_read_u32(dev, "adi,fet-bad-timeout-ms", &val);
	if (!ret) {
		if (val > LTC4282_FET_BAD_MAX_TIMEOUT)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid value(%u) for adi,fet-bad-timeout-ms", val);

		ret = regmap_write(st->map, LTC4282_FET_BAD_FAULT_TIMEOUT, val);
		if (ret)
			return ret;
	}

	return ltc4282_gpio_setup(st);
}

static bool ltc4282_readable_reg(struct device *dev, unsigned int reg)
{
	if (reg == LTC4282_RESERVED_1 || reg == LTC4282_RESERVED_2)
		return false;

	return true;
}

static bool ltc4282_writable_reg(struct device *dev, unsigned int reg)
{
	if (reg == LTC4282_STATUS_LSB || reg == LTC4282_STATUS_MSB)
		return false;
	if (reg == LTC4282_RESERVED_1 || reg == LTC4282_RESERVED_2)
		return false;

	return true;
}

static const struct regmap_config ltc4282_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LTC4282_RESERVED_3,
	.readable_reg = ltc4282_readable_reg,
	.writeable_reg = ltc4282_writable_reg,
};

static const struct hwmon_channel_info * const ltc4282_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LOWEST | HWMON_I_HIGHEST |
			   HWMON_I_MAX | HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX_ALARM | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LCRIT_ALARM | HWMON_I_CRIT_ALARM |
			   HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LOWEST | HWMON_I_HIGHEST |
			   HWMON_I_MAX | HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX_ALARM | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LOWEST | HWMON_C_HIGHEST |
			   HWMON_C_MAX | HWMON_C_MIN | HWMON_C_MIN_ALARM |
			   HWMON_C_MAX_ALARM | HWMON_C_CRIT_ALARM |
			   HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_INPUT_LOWEST |
			   HWMON_P_INPUT_HIGHEST | HWMON_P_MAX | HWMON_P_MIN |
			   HWMON_P_AVERAGE | HWMON_P_MAX_ALARM | HWMON_P_MIN_ALARM |
			   HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(energy,
			   HWMON_E_ENABLE),
	NULL
};

static const struct hwmon_ops ltc4282_hwmon_ops = {
	.read = ltc4282_read,
	.write = ltc4282_write,
	.is_visible = ltc4282_is_visible,
	.read_string = ltc4282_read_labels,
};

static const struct hwmon_chip_info ltc2947_chip_info = {
	.ops = &ltc4282_hwmon_ops,
	.info = ltc4282_info,
};

/* energy attributes are 6bytes wide so we need u64 */
static SENSOR_DEVICE_ATTR(energy1_input, 0444, ltc4282_show_energy, NULL, 0);
/* power1_fault */
static SENSOR_DEVICE_ATTR(power1_good, 0444, ltc4282_show_status, NULL,
			  LTC4282_POWER_GOOD_MASK);
/* FET faults */
static SENSOR_DEVICE_ATTR(fet_short_fault, 0444, ltc4282_show_status, NULL,
			  LTC4282_FET_SHORT_MASK);
static SENSOR_DEVICE_ATTR(fet_bad_fault, 0444, ltc4282_show_status, NULL,
			  LTC4282_FET_BAD_STATUS_MASK);
/*
 * Fault log failures. These faults might be important in systems where auto-retry is not enabled
 * since they will cause the part to latch off until they are cleared. Typically that happens
 * when the system admin is close enough so he can check what happened and manually clear the
 * faults. Moreover, manually clearing the faults might only matter when ON_FAULT_MASK in the
 * CONTROL register is set (which is the default) as in that case, a turn off signal from the
 * ON pin won't clear them.
 */
static SENSOR_DEVICE_ATTR(in1_crit_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_OV_FAULT_MASK);
static SENSOR_DEVICE_ATTR(in1_lcrit_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_UV_FAULT_MASK);
static SENSOR_DEVICE_ATTR(curr1_crit_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_OC_FAULT_MASK);
static SENSOR_DEVICE_ATTR(power1_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_POWER_BAD_FAULT_MASK);
static SENSOR_DEVICE_ATTR(fet_bad_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_FET_BAD_FAULT_MASK);
static SENSOR_DEVICE_ATTR(fet_short_fault_log, 0644, ltc4282_show_fault_log,
			  ltc4282_clear_fault_log, LTC4282_FET_SHORT_FAULT_MASK);

static struct attribute *ltc4282_attrs[] = {
	&sensor_dev_attr_energy1_input.dev_attr.attr,
	&sensor_dev_attr_power1_good.dev_attr.attr,
	&sensor_dev_attr_fet_bad_fault.dev_attr.attr,
	&sensor_dev_attr_fet_short_fault.dev_attr.attr,
	&sensor_dev_attr_in1_crit_fault_log.dev_attr.attr,
	&sensor_dev_attr_in1_lcrit_fault_log.dev_attr.attr,
	&sensor_dev_attr_curr1_crit_fault_log.dev_attr.attr,
	&sensor_dev_attr_power1_fault_log.dev_attr.attr,
	&sensor_dev_attr_fet_bad_fault_log.dev_attr.attr,
	&sensor_dev_attr_fet_short_fault_log.dev_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ltc4282);

static int ltc4282_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev, *hwmon;
	struct ltc4282_state *st;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return dev_err_probe(dev, -ENOMEM, "Failed to allocate memory\n");

	st->dev = dev;
	st->map = devm_regmap_init_i2c(i2c, &ltc4282_regmap_config);
	if (IS_ERR(st->map))
		return dev_err_probe(dev, PTR_ERR(st->map), "failed regmap init\n");

	ret = ltc4282_power_on(st);
	if (ret)
		return ret;

	/* Soft reset */
	ret = regmap_set_bits(st->map, LTC4282_ADC_CTRL, LTC4282_RESET_MASK);
	if (ret)
		return ret;

	msleep(3200);

	ret = ltc428_clks_setup(st);
	if (ret)
		return ret;

	ret = ltc4282_setup(st);
	if (ret)
		return ret;

	mutex_init(&st->lock);
	hwmon = devm_hwmon_device_register_with_info(dev, "ltc4282", st, &ltc2947_chip_info,
						     ltc4282_groups);
	return PTR_ERR_OR_ZERO(hwmon);
}

static const struct of_device_id ltc4282_of_match[] = {
	{ .compatible = "adi,ltc4282" },
	{}
};
MODULE_DEVICE_TABLE(of, ltc4282_of_match);

static struct i2c_driver ltc4282_driver = {
	.driver = {
		.name = "ltc4282",
		.of_match_table = ltc4282_of_match,
	},
	.probe = ltc4282_probe,
};
module_i2c_driver(ltc4282_driver);

MODULE_AUTHOR("Nuno Sa <nuno.sa@analog.com>");
MODULE_DESCRIPTION("LTC4282 I2C High Current Hot Swap Controller");
MODULE_LICENSE("GPL");
