// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices LTC4282 I2C High Current Hot Swap Controller over I2C
 *
 * Copyright 2023 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/units.h>
#include <linux/util_macros.h>

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
#define LTC4282_ALERT_CTRL			0x1c
  #define LTC4282_ALERT_OUT_MASK		BIT(6)
#define LTC4282_ADC_CTRL			0x1d
  #define LTC4282_FAULT_LOG_EN_MASK		BIT(2)
  #define LTC4282_METER_HALT_MASK		BIT(5)
  #define LTC4282_METER_RESET_MASK		BIT(6)
  #define LTC4282_RESET_MASK			BIT(7)
#define LTC4282_STATUS_LSB			0x1e
  #define LTC4282_OV_STATUS_MASK		BIT(0)
  #define LTC4282_UV_STATUS_MASK		BIT(1)
  #define LTC4282_VDD_STATUS_MASK \
		(LTC4282_OV_STATUS_MASK | LTC4282_UV_STATUS_MASK)
  #define LTC4282_OC_STATUS_MASK		BIT(2)
  #define LTC4282_POWER_GOOD_MASK		BIT(3)
  #define LTC4282_FET_SHORT_MASK		BIT(5)
  #define LTC4282_FET_BAD_STATUS_MASK		BIT(6)
#define LTC4282_STATUS_MSB			0x1f
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
#define LTC4282_CLKIN_RANGE	(LTC4282_CLKIN_MAX - LTC4282_CLKIN_MIN + 1)
#define LTC4282_CLKOUT_SYSTEM	(250 * KILO)
#define LTC4282_CLKOUT_CNV	15

#define LTC4282_GPIO_NR		4
/*
 * relaxed version of FIELD_PREP() to be used when mask is not a compile time
 * constant u32_encode_bits() can't also be used as the compiler needs to be
 * able to evaluate mask at compile time.
 */
#define field_prep(_mask, _val)	(((_val) << (ffs(_mask) - 1)) & (_mask))

enum {
	LTC4282_CHAN_VSOURCE,
	LTC4282_CHAN_VDD,
	LTC4282_CHAN_VGPIO,
};

struct ltc4282_cache {
	u32 in_max_raw;
	u32 in_min_raw;
	long in_highest;
	long in_lowest;
	bool en;
};

struct ltc4282_state {
	struct regmap *map;
	/* Protect against multiple accesses to the device registers */
	struct mutex lock;
	struct gpio_chip gc;
	struct clk_hw clk_hw;
	/*
	 * Used to cache values for VDD/VSOURCE depending which will be used
	 * when hwmon is not enabled for that channel. Needed because they share
	 * the same registers.
	 */
	struct ltc4282_cache in0_1_cache[LTC4282_CHAN_VGPIO];
	unsigned long valid_mask;
	long power_max;
	u32 rsense;
	u32 vin_mode;
	u16 vfs_out;
	bool energy_en;
};

struct ltc4282_gpio {
	const char * const *funcs;
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
	LTC4282_GPIO_1,
	LTC4282_GPIO_2,
	LTC4282_GPIO_3,
	LTC4282_ALERT,
	LTC4282_GPIO_MAX
};

static const char * const ltc4282_gpio1_modes[] = {
	"gpio", "power_bad", "power_good"
};

static const char * const ltc4282_gpio2_modes[] = {
	"gpio", "adc_input", "stress_fet"
};

static const char * const ltc4282_gpio3_modes[] = {
	"gpio", "adc_input"
};

static const struct ltc4282_gpio ltc4282_gpios[] = {
	[LTC4282_GPIO_1] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_1_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_1_OUT_MASK,
		.active_high = true,
		.funcs = ltc4282_gpio1_modes,
		.n_funcs = ARRAY_SIZE(ltc4282_gpio1_modes),
	},
	[LTC4282_GPIO_2] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_2_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_2_OUT_MASK,
		.funcs = ltc4282_gpio2_modes,
		.n_funcs = ARRAY_SIZE(ltc4282_gpio2_modes),
	},
	[LTC4282_GPIO_3] = {
		.in_reg = LTC4282_STATUS_MSB,
		.in_mask = LTC4282_GPIO_3_STATUS_MASK,
		.out_reg = LTC4282_GPIO_CONFIG,
		.out_mask = LTC4282_GPIO_3_OUT_MASK,
		.funcs = ltc4282_gpio3_modes,
		.n_funcs = ARRAY_SIZE(ltc4282_gpio3_modes),
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

	/* we can only control this for GPIO_1 */
	if (offset != LTC4282_GPIO_1)
		return 0;

	return regmap_set_bits(st->map, LTC4282_GPIO_CONFIG,
			       LTC4282_GPIO_1_CONFIG_MASK);
}

static int ltc4282_gpio_output_set(struct gpio_chip *chip, unsigned int offset,
				   int val)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[offset];

	guard(mutex)(&st->lock);
	/*
	 * Explicitly setting the pin as output can only be done for GPIO_1. For
	 * the other pins we just pull the line down or high-z.
	 */
	if (offset == LTC4282_GPIO_1) {
		int ret;

		ret = regmap_update_bits(st->map, LTC4282_GPIO_CONFIG,
					 LTC4282_GPIO_1_CONFIG_MASK,
					 FIELD_PREP(LTC4282_GPIO_1_CONFIG_MASK, 2));
		if (ret)
			return ret;
	}

	/*
	 * GPIO_2,3 and the ALERT pin require setting the bit to 1 to pull down
	 * the line
	 */
	if (!gpio->active_high)
		val = !val;

	return regmap_update_bits(st->map, gpio->out_reg, gpio->out_mask,
				  field_prep(gpio->out_mask, val));
}

static void ltc4282_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int val)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[offset];

	if (!gpio->active_high)
		val = !val;

	regmap_update_bits(st->map, gpio->out_reg, gpio->out_mask,
			   field_prep(gpio->out_mask, val));
}

static int ltc4282_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);
	const struct ltc4282_gpio *gpio = &ltc4282_gpios[offset];
	int ret;
	u32 val;

	ret = regmap_read(st->map, gpio->in_reg, &val);
	if (ret)
		return ret;

	return !!(val & gpio->in_mask);
}

static int ltc4282_gpio_valid_mask(struct gpio_chip *chip,
				   unsigned long *valid_mask,
				   unsigned int ngpios)
{
	struct ltc4282_state *st = gpiochip_get_data(chip);

	*valid_mask = st->valid_mask;
	return 0;
}

enum {
	LTC4282_CLKOUT_NONE,
	LTC4282_CLKOUT_INT,
	LTC4282_CLKOUT_TICK,
};

static int ltc4282_set_rate(struct clk_hw *hw,
			    unsigned long rate, unsigned long parent_rate)
{
	struct ltc4282_state *st = container_of(hw, struct ltc4282_state,
						clk_hw);
	u32 val = LTC4282_CLKOUT_INT;

	if (rate == LTC4282_CLKOUT_CNV)
		val = LTC4282_CLKOUT_TICK;

	return regmap_update_bits(st->map, LTC4282_CLK_DIV, LTC4282_CLKOUT_MASK,
				  FIELD_PREP(LTC4282_CLKOUT_MASK, val));
}

/*
 * Note the 15HZ conversion rate assumes 12bit ADC which is what we are
 * supporting for now.
 */
static unsigned int ltc4282_out_rates[] = {
	LTC4282_CLKOUT_CNV, LTC4282_CLKOUT_SYSTEM
};

long ltc4282_round_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long *parent_rate)
{
	int idx = find_closest(rate, ltc4282_out_rates,
			       ARRAY_SIZE(ltc4282_out_rates));

	return ltc4282_out_rates[idx];
}

unsigned long ltc4282_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	struct ltc4282_state *st = container_of(hw, struct ltc4282_state,
						clk_hw);
	u32 clkdiv;
	int ret;

	ret = regmap_read(st->map, LTC4282_CLK_DIV, &clkdiv);
	if (ret)
		return 0;

	clkdiv = FIELD_GET(LTC4282_CLKOUT_MASK, clkdiv);
	if (!clkdiv)
		return 0;
	if (clkdiv == LTC4282_CLKOUT_INT)
		return LTC4282_CLKOUT_SYSTEM;

	return LTC4282_CLKOUT_CNV;
}

static void ltc4282_disable(struct clk_hw *clk_hw)
{
	struct ltc4282_state *st = container_of(clk_hw, struct ltc4282_state,
						clk_hw);

	regmap_clear_bits(st->map, LTC4282_CLK_DIV, LTC4282_CLKOUT_MASK);
}

static int ltc4282_read_voltage_word(const struct ltc4282_state *st, u32 reg,
				     u32 fs, long *val)
{
	__be16 in;
	int ret;

	ret = regmap_bulk_read(st->map, reg, &in, sizeof(in));
	if (ret)
		return ret;

	/*
	 * This is also used to calculate current in which case fs comes in
	 * 10 * uV. Hence the ULL usage.
	 */
	*val = DIV_ROUND_CLOSEST_ULL(be16_to_cpu(in) * (u64)fs, U16_MAX);
	return 0;
}

static int ltc4282_read_voltage_byte_cached(const struct ltc4282_state *st,
					    u32 reg, u32 fs, long *val,
					    u32 *cached_raw)
{
	int ret;
	u32 in;

	if (cached_raw) {
		in = *cached_raw;
	} else {
		ret = regmap_read(st->map, reg, &in);
		if (ret)
			return ret;
	}

	*val = DIV_ROUND_CLOSEST(in * fs, U8_MAX);
	return 0;
}

static int ltc4282_read_voltage_byte(const struct ltc4282_state *st, u32 reg,
				     u32 fs, long *val)
{
	return ltc4282_read_voltage_byte_cached(st, reg, fs, val, NULL);
}

static int __ltc4282_read_alarm(struct ltc4282_state *st, u32 reg, u32 mask,
				long *val)
{
	u32 alarm;
	int ret;

	ret = regmap_read(st->map, reg, &alarm);
	if (ret)
		return ret;

	*val = !!(alarm & mask);

	/* if not status/fault logs, clear the alarm after reading it */
	if (reg != LTC4282_STATUS_LSB && reg != LTC4282_FAULT_LOG)
		return regmap_clear_bits(st->map, reg, mask);

	return 0;
}

static int ltc4282_read_alarm(struct ltc4282_state *st, u32 reg, u32 mask,
			      long *val)
{
	guard(mutex)(&st->lock);
	return __ltc4282_read_alarm(st, reg, mask, val);
}

static int ltc4282_vdd_source_read_in(struct ltc4282_state *st, u32 channel,
				      long *val)
{
	guard(mutex)(&st->lock);
	if (!st->in0_1_cache[channel].en)
		return -ENODATA;

	return ltc4282_read_voltage_word(st, LTC4282_VSOURCE, st->vfs_out, val);
}

static int ltc4282_vdd_source_read_hist(struct ltc4282_state *st, u32 reg,
					u32 channel, long *cached, long *val)
{
	int ret;

	guard(mutex)(&st->lock);
	if (!st->in0_1_cache[channel].en) {
		*val = *cached;
		return 0;
	}

	ret = ltc4282_read_voltage_word(st, reg, st->vfs_out, val);
	if (ret)
		return ret;

	*cached = *val;
	return 0;
}

static int ltc4282_vdd_source_read_lim(struct ltc4282_state *st, u32 reg,
				       u32 channel, u32 *cached, long *val)
{
	guard(mutex)(&st->lock);
	if (!st->in0_1_cache[channel].en)
		return ltc4282_read_voltage_byte_cached(st, reg, st->vfs_out,
							val, cached);

	return ltc4282_read_voltage_byte(st, reg, st->vfs_out, val);
}

static int ltc4282_vdd_source_read_alm(struct ltc4282_state *st, u32 mask,
				       u32 channel, long *val)
{
	guard(mutex)(&st->lock);
	if (!st->in0_1_cache[channel].en) {
		/*
		 * Do this otherwise alarms can get confused because we clear
		 * them after reading them. So, if someone mistakenly reads
		 * VSOURCE right before VDD (or the other way around), we might
		 * get no alarm just because it was cleared when reading VSOURCE
		 * and had no time for a new conversion and thus having the
		 * alarm again.
		 */
		*val = 0;
		return 0;
	}

	return __ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG, mask, val);
}

static int ltc4282_read_in(struct ltc4282_state *st, u32 attr, long *val,
			   u32 channel)
{
	switch (attr) {
	case hwmon_in_input:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_read_voltage_word(st, LTC4282_VGPIO,
							 1280, val);

		return ltc4282_vdd_source_read_in(st, channel, val);
	case hwmon_in_highest:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_read_voltage_word(st,
							 LTC4282_VGPIO_HIGHEST,
							 1280, val);

		return ltc4282_vdd_source_read_hist(st, LTC4282_VSOURCE_HIGHEST,
						    channel,
						    &st->in0_1_cache[channel].in_highest, val);
	case hwmon_in_lowest:
		if (channel == LTC4282_CHAN_VGPIO)
			ltc4282_read_voltage_word(st, LTC4282_VGPIO_LOWEST,
						  1280, val);

		return ltc4282_vdd_source_read_hist(st, LTC4282_VSOURCE_LOWEST,
						    channel,
						    &st->in0_1_cache[channel].in_lowest, val);
	case hwmon_in_max_alarm:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
						  LTC4282_GPIO_ALARM_H_MASK,
						  val);

		return ltc4282_vdd_source_read_alm(st,
						   LTC4282_VSOURCE_ALARM_H_MASK,
						   channel, val);
	case hwmon_in_min_alarm:
		if (channel == LTC4282_CHAN_VGPIO)
			ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
					   LTC4282_GPIO_ALARM_L_MASK, val);

		return ltc4282_vdd_source_read_alm(st,
						   LTC4282_VSOURCE_ALARM_L_MASK,
						   channel, val);
	case hwmon_in_crit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB,
					  LTC4282_OV_STATUS_MASK, val);
	case hwmon_in_lcrit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB,
					  LTC4282_UV_STATUS_MASK, val);
	case hwmon_in_max:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_read_voltage_byte(st, LTC4282_VGPIO_MAX,
							 1280, val);

		return ltc4282_vdd_source_read_lim(st, LTC4282_VSOURCE_MAX,
						   channel,
						   &st->in0_1_cache[channel].in_max_raw, val);
	case hwmon_in_min:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_read_voltage_byte(st, LTC4282_VGPIO_MIN,
							 1280, val);

		return ltc4282_vdd_source_read_lim(st, LTC4282_VSOURCE_MIN,
						   channel,
						   &st->in0_1_cache[channel].in_min_raw, val);
	case hwmon_in_enable:
		scoped_guard(mutex, &st->lock) {
			*val = st->in0_1_cache[channel].en;
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_read_current_word(const struct ltc4282_state *st, u32 reg,
				     long *val)
{
	long in;
	int ret;

	/*
	 * We pass in full scale in 10 * micro (note that 40 is already
	 * millivolt) so we have better approximations to calculate current.
	 */
	ret = ltc4282_read_voltage_word(st, reg, DECA * 40 * MILLI, &in);
	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(in * MILLI, st->rsense);

	return 0;
}

static int ltc4282_read_current_byte(const struct ltc4282_state *st, u32 reg,
				     long *val)
{
	long in;
	int ret;

	ret = ltc4282_read_voltage_byte(st, reg, DECA * 40 * MILLI, &in);
	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(in * MILLI, st->rsense);

	return 0;
}

static int ltc4282_read_curr(struct ltc4282_state *st, const u32 attr,
			     long *val)
{
	switch (attr) {
	case hwmon_curr_input:
		return ltc4282_read_current_word(st, LTC4282_VSENSE, val);
	case hwmon_curr_highest:
		return ltc4282_read_current_word(st, LTC4282_VSENSE_HIGHEST,
						 val);
	case hwmon_curr_lowest:
		return ltc4282_read_current_word(st, LTC4282_VSENSE_LOWEST,
						 val);
	case hwmon_curr_max:
		return ltc4282_read_current_byte(st, LTC4282_VSENSE_MAX, val);
	case hwmon_curr_min:
		return ltc4282_read_current_byte(st, LTC4282_VSENSE_MIN, val);
	case hwmon_curr_max_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
					  LTC4282_VSENSE_ALARM_H_MASK, val);
	case hwmon_curr_min_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
					  LTC4282_VSENSE_ALARM_L_MASK, val);
	case hwmon_curr_crit_alarm:
		return ltc4282_read_alarm(st, LTC4282_STATUS_LSB,
					  LTC4282_OC_STATUS_MASK, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_read_power_word(const struct ltc4282_state *st, u32 reg,
				   long *val)
{
	u64 temp =  DECA * 40ULL * st->vfs_out * BIT(16), temp_2;
	__be16 raw;
	u16 power;
	int ret;

	ret = regmap_bulk_read(st->map, reg, &raw, sizeof(raw));
	if (ret)
		return ret;

	power = be16_to_cpu(raw);
	/*
	 * Power is given by:
	 *     P = CODE(16b) * 0.040 * Vfs(out) * 2^16 / ((2^16 - 1)^2 * Rsense)
	 */
	if (check_mul_overflow(power * temp, MICRO, &temp_2)) {
		temp = DIV_ROUND_CLOSEST_ULL(power * temp, U16_MAX);
		*val = DIV64_U64_ROUND_CLOSEST(temp * MICRO,
					       U16_MAX * (u64)st->rsense);
		return 0;
	}

	*val = DIV64_U64_ROUND_CLOSEST(temp_2,
				       st->rsense * int_pow(U16_MAX, 2));

	return 0;
}

static int ltc4282_read_power_byte(const struct ltc4282_state *st, u32 reg,
				   long *val)
{
	u32 power;
	u64 temp;
	int ret;

	ret = regmap_read(st->map, reg, &power);
	if (ret)
		return ret;

	temp = power * 40 * DECA * st->vfs_out * BIT_ULL(8);
	*val = DIV64_U64_ROUND_CLOSEST(temp * MICRO,
				       int_pow(U8_MAX, 2) * st->rsense);

	return 0;
}

static int ltc4282_read_energy(const struct ltc4282_state *st, u64 *val)
{
	u64 temp, energy;
	__be64 raw;
	int ret;

	ret = regmap_bulk_read(st->map, LTC4282_ENERGY, &raw, 6);
	if (ret)
		return ret;

	energy =  be64_to_cpu(raw) >> 16;
	/*
	 * The formula for energy is given by:
	 *	E = CODE(48b) * 0.040 * Vfs(out) * Tconv * 256 /
	 *						((2^16 - 1)^2 * Rsense)
	 *
	 * Since we only support 12bit ADC, Tconv = 0.065535s. Passing Vfs(out)
	 * and 0.040 to mV and Tconv to us, we can simplify the formula to:
	 *	E = CODE(48b) * 40 * Vfs(out) * 256 / (U16_MAX * Rsense)
	 *
	 * As Rsense can have tenths of micro-ohm resolution, we need to
	 * multiply by DECA to get microujoule.
	 */
	if (check_mul_overflow(DECA * st->vfs_out * 40 * BIT(8), energy, &temp)) {
		temp = DIV_ROUND_CLOSEST(DECA * st->vfs_out * 40 * BIT(8), U16_MAX);
		*val = DIV_ROUND_CLOSEST_ULL(temp * energy, st->rsense);
		return 0;
	}

	*val = DIV64_U64_ROUND_CLOSEST(temp, U16_MAX * (u64)st->rsense);

	return 0;
}

static int ltc4282_read_power(struct ltc4282_state *st, const u32 attr,
			      long *val)
{
	switch (attr) {
	case hwmon_power_input:
		return ltc4282_read_power_word(st, LTC4282_POWER, val);
	case hwmon_power_input_highest:
		return ltc4282_read_power_word(st, LTC4282_POWER_HIGHEST, val);
	case hwmon_power_input_lowest:
		return ltc4282_read_power_word(st, LTC4282_POWER_LOWEST, val);
	case hwmon_power_max_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
					  LTC4282_POWER_ALARM_H_MASK, val);
	case hwmon_power_min_alarm:
		return ltc4282_read_alarm(st, LTC4282_ADC_ALERT_LOG,
					  LTC4282_POWER_ALARM_L_MASK, val);
	case hwmon_power_max:
		return ltc4282_read_power_byte(st, LTC4282_POWER_MAX, val);
	case hwmon_power_min:
		return ltc4282_read_power_byte(st, LTC4282_POWER_MIN, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_in:
		return ltc4282_read_in(st, attr, val, channel);
	case hwmon_curr:
		return ltc4282_read_curr(st, attr, val);
	case hwmon_power:
		return ltc4282_read_power(st, attr, val);
	case hwmon_energy:
		scoped_guard(mutex, &st->lock) {
			*val = st->energy_en;
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_write_power_byte(const struct ltc4282_state *st, u32 reg,
				    long val)
{
	u32 power;
	u64 temp;

	if (val > st->power_max)
		val = st->power_max;

	temp = val * int_pow(U8_MAX, 2) * st->rsense;
	power = DIV64_U64_ROUND_CLOSEST(temp,
					MICRO * DECA * 256ULL * st->vfs_out * 40);

	return regmap_write(st->map, reg, power);
}

static int __ltc4282_write_history(const struct ltc4282_state *st, u32 reg,
				   long lowest, long highest)
{
	__be16 __raw;
	u16 tmp;
	int ret;

	tmp = DIV_ROUND_CLOSEST(U16_MAX * lowest, st->vfs_out);

	__raw = cpu_to_be16(tmp);

	ret = regmap_bulk_write(st->map, reg, &__raw, 2);
	if (ret)
		return ret;

	tmp = DIV_ROUND_CLOSEST(U16_MAX * highest, st->vfs_out);

	__raw = cpu_to_be16(tmp);

	return regmap_bulk_write(st->map, reg + 2, &__raw, 2);
}

static int ltc4282_write_history(struct ltc4282_state *st, u32 reg, long lowest,
				 long highest)
{
	guard(mutex)(&st->lock);
	return __ltc4282_write_history(st, reg, lowest, highest);
}

static int ltc4282_write_power(struct ltc4282_state *st, u32 attr,
			       long val)
{
	switch (attr) {
	case hwmon_power_max:
		return ltc4282_write_power_byte(st, LTC4282_POWER_MAX, val);
	case hwmon_power_min:
		return ltc4282_write_power_byte(st, LTC4282_POWER_MIN, val);
	case hwmon_power_reset_history:
		return ltc4282_write_history(st, LTC4282_POWER_LOWEST, 0, 0);
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_write_voltage_byte_cached(const struct ltc4282_state *st,
					     u32 reg, u32 fs, long val,
					     u32 *cache_raw)
{
	u32 in;

	if (val >= fs)
		in = U8_MAX;
	else
		in = DIV_ROUND_CLOSEST(val * U8_MAX, fs);

	if (cache_raw) {
		*cache_raw = in;
		return 0;
	}

	return regmap_write(st->map, reg, in);
}

static int ltc4282_write_voltage_byte(const struct ltc4282_state *st, u32 reg,
				      u32 fs, long val)
{
	return ltc4282_write_voltage_byte_cached(st, reg, fs, val, NULL);
}

static int ltc4282_cache_history(struct ltc4282_state *st, u32 channel)
{
	long val;
	int ret;

	ret = ltc4282_read_voltage_word(st, LTC4282_VSOURCE_LOWEST, st->vfs_out,
					&val);
	if (ret)
		return ret;

	st->in0_1_cache[channel].in_lowest = val;

	ret = ltc4282_read_voltage_word(st, LTC4282_VSOURCE_HIGHEST,
					st->vfs_out, &val);
	if (ret)
		return ret;

	st->in0_1_cache[channel].in_highest = val;

	ret = regmap_read(st->map, LTC4282_VSOURCE_MIN,
			  &st->in0_1_cache[channel].in_min_raw);
	if (ret)
		return ret;

	return regmap_read(st->map, LTC4282_VSOURCE_MAX,
			  &st->in0_1_cache[channel].in_max_raw);
}

static int ltc4282_cache_sync(struct ltc4282_state *st, u32 channel)
{
	int ret;

	ret = __ltc4282_write_history(st, LTC4282_VSOURCE_LOWEST,
				      st->in0_1_cache[channel].in_lowest,
				      st->in0_1_cache[channel].in_highest);
	if (ret)
		return ret;

	ret = regmap_write(st->map, LTC4282_VSOURCE_MIN,
			   st->in0_1_cache[channel].in_min_raw);

	return regmap_write(st->map, LTC4282_VSOURCE_MAX,
			    st->in0_1_cache[channel].in_max_raw);
}

static int ltc4282_vdd_source_write_lim(struct ltc4282_state *st, u32 reg,
					int channel, u32 *cache, long val)
{
	int ret;

	guard(mutex)(&st->lock);
	if (st->in0_1_cache[channel].en)
		ret = ltc4282_write_voltage_byte(st, reg, st->vfs_out, val);
	else
		ret = ltc4282_write_voltage_byte_cached(st, reg, st->vfs_out,
							val, cache);

	return ret;
}

static int ltc4282_vdd_source_write_hist(struct ltc4282_state *st, int channel)
{
	int ret;

	guard(mutex)(&st->lock);
	if (st->in0_1_cache[channel].en) {
		ret = __ltc4282_write_history(st, LTC4282_VSOURCE_LOWEST, 0, 0);
		if (ret)
			return ret;
	}

	st->in0_1_cache[channel].in_lowest = 0;
	st->in0_1_cache[channel].in_highest = 0;

	return 0;
}

/*
 * We need to mux between VSOURCE and VDD which means they are mutually
 * exclusive. Moreover, we can't really disable both VDD and VSOURCE as the ADC
 * is continuously running (we cannot independently halt it without also
 * stopping VGPIO). Hence, the logic is that disabling or enabling VDD will
 * automatically have the reverse effect on VSOURCE and vice-versa.
 */
static int ltc4282_vdd_source_enable(struct ltc4282_state *st, int channel,
				     long val)
{
	int ret, other_chan = ~channel & 0x1;
	u8 __val = val;

	guard(mutex)(&st->lock);
	if (st->in0_1_cache[channel].en == !!val)
		return 0;

	/* clearing the bit makes the ADC to monitor VDD */
	if (channel == LTC4282_CHAN_VDD)
		__val = !__val;

	ret = regmap_update_bits(st->map, LTC4282_ILIM_ADJUST,
				 LTC4282_VDD_MONITOR_MASK,
				 FIELD_PREP(LTC4282_VDD_MONITOR_MASK, !!__val));
	if (ret)
		return ret;

	st->in0_1_cache[channel].en = !!val;
	st->in0_1_cache[other_chan].en = !val;

	if (st->in0_1_cache[channel].en) {
		/*
		 * Then, we are disabling @other_chan. Let's save it's current
		 * history.
		 */
		ret = ltc4282_cache_history(st, other_chan);
		if (ret)
			return ret;

		return ltc4282_cache_sync(st, channel);
	}
	/*
	 * Then, we are enabling @other_chan. We need to do the opposite from
	 * above.
	 */
	ret = ltc4282_cache_history(st, channel);
	if (ret)
		return ret;

	return ltc4282_cache_sync(st, other_chan);
}

static int ltc4282_write_in(struct ltc4282_state *st, u32 attr, long val,
			    int channel)
{
	switch (attr) {
	case hwmon_in_max:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_write_voltage_byte(st, LTC4282_VGPIO_MAX,
							  1280, val);

		return ltc4282_vdd_source_write_lim(st, LTC4282_VSOURCE_MAX,
						    channel,
						    &st->in0_1_cache[channel].in_max_raw, val);
	case hwmon_in_min:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_write_voltage_byte(st, LTC4282_VGPIO_MIN,
							  1280, val);

		return ltc4282_vdd_source_write_lim(st, LTC4282_VSOURCE_MIN,
						    channel,
						    &st->in0_1_cache[channel].in_min_raw, val);
	case hwmon_in_reset_history:
		if (channel == LTC4282_CHAN_VGPIO)
			return ltc4282_write_history(st, LTC4282_VGPIO_LOWEST,
						     0, 0);

		return ltc4282_vdd_source_write_hist(st, channel);
	case hwmon_in_enable:
		return ltc4282_vdd_source_enable(st, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_write_curr(struct ltc4282_state *st, u32 attr,
			      long val)
{
	/* need to pass it in millivolt */
	u32 in = DIV_ROUND_CLOSEST_ULL((u64)val * st->rsense, DECA * MICRO);

	switch (attr) {
	case hwmon_curr_max:
		return ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MAX, 40,
						  in);
	case hwmon_curr_min:
		return ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MIN, 40,
						  in);
	case hwmon_curr_reset_history:
		return ltc4282_write_history(st, LTC4282_VSENSE_LOWEST, 0, 0);
	default:
		return -EOPNOTSUPP;
	}
}

static int ltc4282_energy_enable_set(struct ltc4282_state *st, long val)
{
	int ret;

	guard(mutex)(&st->lock);
	/* setting the bit halts the meter */
	ret = regmap_update_bits(st->map, LTC4282_ADC_CTRL,
				 LTC4282_METER_HALT_MASK,
				 FIELD_PREP(LTC4282_METER_HALT_MASK, !val));
	if (ret)
		return ret;

	st->energy_en = !!val;

	return 0;
}

static int ltc4282_write(struct device *dev,
			 enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_power:
		return ltc4282_write_power(st, attr, val);
	case hwmon_in:
		return ltc4282_write_in(st, attr, val, channel);
	case hwmon_curr:
		return ltc4282_write_curr(st, attr, val);
	case hwmon_energy:
		return ltc4282_energy_enable_set(st, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t ltc4282_in_is_visible(const struct ltc4282_state *st, u32 attr)
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
	case hwmon_in_enable:
	case hwmon_in_reset_history:
		return 0644;
	default:
		return 0;
	}
}

static umode_t ltc4282_curr_is_visible(u32 attr)
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
	case hwmon_curr_reset_history:
		return 0644;
	default:
		return 0;
	}
}

static umode_t ltc4282_power_is_visible(u32 attr)
{
	switch (attr) {
	case hwmon_power_input:
	case hwmon_power_input_highest:
	case hwmon_power_input_lowest:
	case hwmon_power_label:
	case hwmon_power_max_alarm:
	case hwmon_power_min_alarm:
		return 0444;
	case hwmon_power_max:
	case hwmon_power_min:
	case hwmon_power_reset_history:
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
		return 0;
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
		return -EOPNOTSUPP;
	}
}

static ssize_t ltc4282_energy_show(struct device *dev,
				   struct device_attribute *da, char *buf)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	u64 energy;
	int ret;

	guard(mutex)(&st->lock);
	if (!st->energy_en)
		return -ENODATA;

	ret = ltc4282_read_energy(st, &energy);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%llu\n", energy);
}

static ssize_t ltc4282_status_show(struct device *dev,
				   struct device_attribute *da, char *buf)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	long alarm;
	int ret;

	ret = ltc4282_read_alarm(st, LTC4282_STATUS_LSB, attr->index, &alarm);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", alarm);
}

static ssize_t ltc4282_clear_faults_store(struct device *dev,
					  struct device_attribute *da,
					  const char *buf, size_t len)
{
	struct ltc4282_state *st = dev_get_drvdata(dev);
	int ret;

	ret = regmap_write(st->map, LTC4282_FAULT_LOG, 0);
	if (ret)
		return ret;

	return len;
}

static const struct clk_ops ltc4282_ops = {
	.recalc_rate = ltc4282_recalc_rate,
	.round_rate = ltc4282_round_rate,
	.set_rate = ltc4282_set_rate,
	.disable = ltc4282_disable,
};

static int ltc428_clk_provider_setup(struct ltc4282_state *st,
				     struct device *dev)
{
	struct clk_init_data init;
	int ret;

	if (!IS_ENABLED(CONFIG_COMMON_CLK))
		return 0;

	init.name = "ltc4282-clk";
	init.ops = &ltc4282_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	st->clk_hw.init = &init;

	ret = devm_clk_hw_register(dev, &st->clk_hw);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &st->clk_hw);
}

static int ltc428_clks_setup(struct ltc4282_state *st, struct device *dev)
{
	unsigned long rate;
	struct clk *clkin;
	u32 val;
	int ret;

	ret = ltc428_clk_provider_setup(st, dev);
	if (ret)
		return ret;

	clkin = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(clkin))
		return dev_err_probe(dev, PTR_ERR(clkin),
				     "Failed to get clkin");
	if (!clkin)
		return 0;

	rate = clk_get_rate(clkin);
	if (in_range(rate, LTC4282_CLKIN_MIN, LTC4282_CLKIN_RANGE))
		return dev_err_probe(dev, -EINVAL,
				     "Invalid clkin range(%lu) [%lu %lu]\n",
				     rate, LTC4282_CLKIN_MIN,
				     LTC4282_CLKIN_MAX);

	/*
	 * Clocks faster than 250KHZ should be reduced to 250KHZ. The clock
	 * frequency is divided by twice the value in the register.
	 */
	val = rate / (2 * LTC4282_CLKIN_MIN);

	return regmap_update_bits(st->map, LTC4282_CLK_DIV,
				  LTC4282_CLK_DIV_MASK,
				  FIELD_PREP(LTC4282_CLK_DIV_MASK, val));
}

static const int ltc4282_curr_lim_uv[] = {
	12500, 15625, 18750, 21875, 25000, 28125, 31250, 34375
};

static int ltc4282_get_defaults(struct ltc4282_state *st, u32 *curr_lim_uv)
{
	u32 reg_val, ilm_adjust;
	int ret;

	ret = regmap_read(st->map, LTC4282_ADC_CTRL, &reg_val);
	if (ret)
		return ret;

	st->energy_en = !FIELD_GET(LTC4282_METER_HALT_MASK, reg_val);

	ret = regmap_read(st->map, LTC4282_CTRL_MSB, &reg_val);
	if (ret)
		return ret;

	st->vin_mode = FIELD_GET(LTC4282_CTRL_VIN_MODE_MASK, reg_val);

	ret = regmap_read(st->map, LTC4282_ILIM_ADJUST, &reg_val);
	if (ret)
		return ret;

	ilm_adjust = FIELD_GET(LTC4282_ILIM_ADJUST_MASK, reg_val);
	*curr_lim_uv = ltc4282_curr_lim_uv[ilm_adjust];

	st->in0_1_cache[LTC4282_CHAN_VSOURCE].en = FIELD_GET(LTC4282_VDD_MONITOR_MASK,
							     ilm_adjust);
	if (!st->in0_1_cache[LTC4282_CHAN_VSOURCE].en) {
		st->in0_1_cache[LTC4282_CHAN_VDD].en = true;
		return regmap_read(st->map, LTC4282_VSOURCE_MAX,
				   &st->in0_1_cache[LTC4282_CHAN_VSOURCE].in_max_raw);
	}

	return regmap_read(st->map, LTC4282_VSOURCE_MAX,
			   &st->in0_1_cache[LTC4282_CHAN_VDD].in_max_raw);
}

/*
 * Set max limits for ISENSE and Power as that depends on the max voltage on
 * rsense that is defined in ILIM_ADJUST. This is specially important for power
 * because for some rsense and vfsout values, if we allow the default raw 255
 * value, that would overflow long in 32bit archs when reading back the max
 * power limit.
 */
static int ltc4282_set_max_limits(struct ltc4282_state *st, u32 val_uv)
{
	int ret;

	ret = ltc4282_write_voltage_byte(st, LTC4282_VSENSE_MAX, 40 * MILLI,
					 val_uv);
	if (ret)
		return ret;

	/* Power is given by ISENSE * Vout. */
	st->power_max = DIV_ROUND_CLOSEST(val_uv * DECA * MILLI, st->rsense) * st->vfs_out;
	return ltc4282_write_power_byte(st, LTC4282_POWER_MAX, st->power_max);
}

static int ltc4282_non_gpio_setup(struct ltc4282_state *st,
				  const struct device *dev, u32 pin,
				  const char *func, bool *adc_in)
{
	if (pin == LTC4282_GPIO_1) {
		/* power_good */
		u32 val = 0;

		if (!strcmp(func, "power_bad"))
			val = 1;

		return regmap_update_bits(st->map, LTC4282_GPIO_CONFIG,
					  LTC4282_GPIO_1_CONFIG_MASK,
					  FIELD_PREP(LTC4282_GPIO_1_CONFIG_MASK, val));
	}

	if (!strcmp(func, "stress_fet"))
		return regmap_update_bits(st->map, LTC4282_GPIO_CONFIG,
					  LTC4282_GPIO_2_FET_STRESS_MASK,
					  FIELD_PREP(LTC4282_GPIO_2_FET_STRESS_MASK, 1));

	/*
	 * Then, let's point the given GPIO to the ADC input. We need to ensure that
	 * function is only given once.
	 */
	if (*adc_in)
		return dev_err_probe(dev, -EINVAL,
				     "Only one gpio can be given to the ADC input\n");

	*adc_in = true;

	/* setting the bit to 1 cause the ADC to monitor GPIO2 */
	return regmap_update_bits(st->map, LTC4282_ILIM_ADJUST,
				  LTC4282_GPIO_MODE_MASK,
				  FIELD_PREP(LTC4282_GPIO_MODE_MASK, pin == LTC4282_GPIO_2));
}

static const char * const ltc4282_gpio_prop[] = {
	"adi,gpio1-mode", "adi,gpio2-mode", "adi,gpio3-mode"
};

static int ltc4282_gpio_setup(struct ltc4282_state *st, struct device *dev)
{
	bool adc_in = false, has_gpio = false;
	const char *func;
	u32 gpio;
	int ret;

	if (!IS_ENABLED(CONFIG_GPIOLIB))
		return 0;

	for (gpio = 0; gpio <= LTC4282_GPIO_3; gpio++) {
		ret = device_property_read_string(dev, ltc4282_gpio_prop[gpio],
						  &func);
		if (ret)
			continue;
		if (match_string(ltc4282_gpios[gpio].funcs,
				 ltc4282_gpios[gpio].n_funcs, func) < 0)
			return dev_err_probe(dev, ret,
					     "Invalid func(%s) for gpio%u\n",
					     func, gpio + 1);
		if (!strcmp(func, "gpio")) {
			__set_bit(gpio, &st->valid_mask);
			has_gpio = true;
			if (gpio == LTC4282_GPIO_1) {
				/* default to input GPIO */
				ret = regmap_set_bits(st->map,
						      LTC4282_GPIO_CONFIG,
						      LTC4282_GPIO_1_CONFIG_MASK);
				if (ret)
					return ret;
			}

			continue;
		}

		ret = ltc4282_non_gpio_setup(st, dev, gpio, func, &adc_in);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,gpio-alert")) {
		__set_bit(LTC4282_ALERT, &st->valid_mask);
		has_gpio = true;
	}

	if (!has_gpio)
		return 0;

	st->gc.parent = dev;
	st->gc.base = -1;
	st->gc.ngpio = LTC4282_GPIO_MAX;
	st->gc.can_sleep = true;
	st->gc.label = "ltc4282";
	st->gc.direction_input = ltc4282_gpio_input_set;
	st->gc.direction_output = ltc4282_gpio_output_set;
	st->gc.set = ltc4282_gpio_set;
	st->gc.get = ltc4282_gpio_get;
	st->gc.init_valid_mask = ltc4282_gpio_valid_mask;

	return devm_gpiochip_add_data(dev, &st->gc, st);
}

static const char * const ltc4282_dividers[] = {
	"external", "vdd_5_percent", "vdd_10_percent", "vdd_15_percent"
};

/* This maps the Vout full scale for the given Vin mode */
static const u16 ltc4282_vfs_milli[] = { 5540, 8320, 16640, 33280 };

static int ltc4282_setup(struct ltc4282_state *st, struct device *dev)
{
	u32 val, curr_lim_uv;
	const char *divider;
	int ret;

	/* The part has an eeprom so let's get the needed defaults from it */
	ret = ltc4282_get_defaults(st, &curr_lim_uv);
	if (ret)
		return ret;

	ret = device_property_read_u32(dev, "adi,rsense-nano-ohms",
				       &st->rsense);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to read adi,rsense-nano-ohms\n");
	if (st->rsense < CENTI)
		return dev_err_probe(dev, -EINVAL,
				     "adi,rsense-nano-ohms too small (< %lu)\n",
				     CENTI);

	/*
	 * The resolution for rsense is tenths of micro (eg: 62.5 uOhm) which
	 * means we need nano in the bindings. However, to make things easier to
	 * handle (with respect to overflows) we divide it by 100 as we don't
	 * really need the last two digits.
	 */
	st->rsense /= CENTI;

	ret = device_property_read_u32(dev, "adi,vin-mode-microvolt", &val);
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
					     "Invalid val(%u) for vin-mode-microvolt\n",
					     val);
		}

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB,
					 LTC4282_CTRL_VIN_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_VIN_MODE_MASK, st->vin_mode));
		if (ret)
			return ret;

		/* Foldback mode should also be set to the input voltage */
		ret = regmap_update_bits(st->map, LTC4282_ILIM_ADJUST,
					 LTC4282_FOLDBACK_MODE_MASK,
					 FIELD_PREP(LTC4282_FOLDBACK_MODE_MASK, st->vin_mode));
		if (ret)
			return ret;
	}

	st->vfs_out = ltc4282_vfs_milli[st->vin_mode];

	ret = device_property_read_u32(dev, "adi,current-limit-sense-microvolt",
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

		ret = regmap_update_bits(st->map, LTC4282_ILIM_ADJUST,
					 LTC4282_ILIM_ADJUST_MASK,
					 FIELD_PREP(LTC4282_ILIM_ADJUST_MASK, reg_val));
		if (ret)
			return ret;
	}

	ret = ltc4282_set_max_limits(st, curr_lim_uv);
	if (ret)
		return ret;

	ret = device_property_read_string(dev, "adi,overvoltage-dividers",
					  &divider);
	if (!ret) {
		int div = match_string(ltc4282_dividers,
				       ARRAY_SIZE(ltc4282_dividers), divider);
		if (div < 0)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid val(%s) for adi,overvoltage-divider\n",
					     divider);

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB,
					 LTC4282_CTRL_OV_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_OV_MODE_MASK, div));
	}

	ret = device_property_read_string(dev, "adi,undervoltage-dividers",
					  &divider);
	if (!ret) {
		int div = match_string(ltc4282_dividers,
				       ARRAY_SIZE(ltc4282_dividers), divider);
		if (div < 0)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid val(%s) for adi,undervoltage-divider\n",
					     divider);

		ret = regmap_update_bits(st->map, LTC4282_CTRL_MSB,
					 LTC4282_CTRL_UV_MODE_MASK,
					 FIELD_PREP(LTC4282_CTRL_UV_MODE_MASK, div));
	}

	if (device_property_read_bool(dev, "adi,overcurrent-retry")) {
		ret = regmap_set_bits(st->map, LTC4282_CTRL_LSB,
				      LTC4282_CTRL_OC_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,overvoltage-retry-disable")) {
		ret = regmap_clear_bits(st->map, LTC4282_CTRL_LSB,
					LTC4282_CTRL_OV_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,undervoltage-retry-disable")) {
		ret = regmap_clear_bits(st->map, LTC4282_CTRL_LSB,
					LTC4282_CTRL_UV_RETRY_MASK);
		if (ret)
			return ret;
	}

	if (device_property_read_bool(dev, "adi,fault-log-enable")) {
		ret = regmap_set_bits(st->map, LTC4282_ADC_CTRL,
				      LTC4282_FAULT_LOG_EN_MASK);
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
					     "Invalid value(%u) for adi,fet-bad-timeout-ms",
					     val);

		ret = regmap_write(st->map, LTC4282_FET_BAD_FAULT_TIMEOUT, val);
		if (ret)
			return ret;
	}

	return ltc4282_gpio_setup(st, dev);
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
			   HWMON_I_MAX_ALARM | HWMON_I_ENABLE |
			   HWMON_I_RESET_HISTORY | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LOWEST | HWMON_I_HIGHEST |
			   HWMON_I_MAX | HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX_ALARM | HWMON_I_LCRIT_ALARM |
			   HWMON_I_CRIT_ALARM | HWMON_I_ENABLE |
			   HWMON_I_RESET_HISTORY | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LOWEST | HWMON_I_HIGHEST |
			   HWMON_I_MAX | HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX_ALARM | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LOWEST | HWMON_C_HIGHEST |
			   HWMON_C_MAX | HWMON_C_MIN | HWMON_C_MIN_ALARM |
			   HWMON_C_MAX_ALARM | HWMON_C_CRIT_ALARM |
			   HWMON_I_RESET_HISTORY | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_INPUT_LOWEST |
			   HWMON_P_INPUT_HIGHEST | HWMON_P_MAX | HWMON_P_MIN |
			   HWMON_P_MAX_ALARM | HWMON_P_MIN_ALARM |
			   HWMON_I_RESET_HISTORY | HWMON_P_LABEL),
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
static SENSOR_DEVICE_ATTR_RO(energy1_input, ltc4282_energy, 0);
/* power1_fault */
static SENSOR_DEVICE_ATTR_RO(power1_good, ltc4282_status,
			     LTC4282_POWER_GOOD_MASK);
/* FET faults */
static SENSOR_DEVICE_ATTR_RO(fet_short_fault, ltc4282_status,
			     LTC4282_FET_SHORT_MASK);
static SENSOR_DEVICE_ATTR_RO(fet_bad_fault, ltc4282_status,
			     LTC4282_FET_BAD_STATUS_MASK);
/*
 * Fault log failures. These faults might be important in systems where
 * auto-retry is not enabled since they will cause the part to latch off until
 * they are cleared. Typically that happens when the system admin is close
 * enough so he can check what happened and manually clear the faults. Hence, we
 * provide an attribute to clear all fauls at once while still capable of
 * checking individual faults in debugfs.
 */
static SENSOR_DEVICE_ATTR_WO(fault_logs_reset, ltc4282_clear_faults, 0);

static struct attribute *ltc4282_attrs[] = {
	&sensor_dev_attr_energy1_input.dev_attr.attr,
	&sensor_dev_attr_power1_good.dev_attr.attr,
	&sensor_dev_attr_fet_bad_fault.dev_attr.attr,
	&sensor_dev_attr_fet_short_fault.dev_attr.attr,
	&sensor_dev_attr_fault_logs_reset.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(ltc4282);

#define LTC4282_DEBUGFS_FAULT_LOG(_name, _mask)					\
static int ltc4282_show_##_name##_fault_log(void *arg, u64 *val)		\
{										\
	struct ltc4282_state *st = arg;						\
	long alarm;								\
	int ret;								\
										\
	ret = ltc4282_read_alarm(st, LTC4282_FAULT_LOG,	_mask, &alarm);		\
	if (ret)								\
		return ret;							\
										\
	*val = alarm;								\
										\
	return 0;								\
}										\
DEFINE_DEBUGFS_ATTRIBUTE(ltc4282_##_name##_fault_log,				\
			 ltc4282_show_##_name##_fault_log, NULL, "%llu\n")

LTC4282_DEBUGFS_FAULT_LOG(curr1_crit, LTC4282_OC_FAULT_MASK);
LTC4282_DEBUGFS_FAULT_LOG(in1_lcrit, LTC4282_UV_FAULT_MASK);
LTC4282_DEBUGFS_FAULT_LOG(in1_crit, LTC4282_OV_FAULT_MASK);
LTC4282_DEBUGFS_FAULT_LOG(fet_bad, LTC4282_FET_BAD_FAULT_MASK);
LTC4282_DEBUGFS_FAULT_LOG(fet_short, LTC4282_FET_SHORT_FAULT_MASK);
LTC4282_DEBUGFS_FAULT_LOG(power1, LTC4282_POWER_BAD_FAULT_MASK);

static void ltc4282_debugfs_remove(void *dir)
{
	debugfs_remove_recursive(dir);
}

static void ltc4282_debugfs_init(struct ltc4282_state *st,
				 struct i2c_client *i2c,
				 const struct device *hwmon)
{
	const char *debugfs_name;
	struct dentry *dentry;
	int ret;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		return;

	debugfs_name = devm_kasprintf(&i2c->dev, GFP_KERNEL, "%s-%s",
				      i2c->name, dev_name(hwmon));
	if (!debugfs_name)
		return;

	dentry = debugfs_create_dir(debugfs_name, NULL);
	if (IS_ERR(dentry))
		return;

	ret = devm_add_action_or_reset(&i2c->dev, ltc4282_debugfs_remove,
				       dentry);
	if (ret)
		return;

	debugfs_create_file_unsafe("power1_fault_log", 0400, dentry, st,
				   &ltc4282_power1_fault_log);
	debugfs_create_file_unsafe("fet_short_fault_log", 0400, dentry, st,
				   &ltc4282_fet_short_fault_log);
	debugfs_create_file_unsafe("fet_bad_fault_log", 0400, dentry, st,
				   &ltc4282_fet_bad_fault_log);
	debugfs_create_file_unsafe("in1_crit_fault_log", 0400, dentry, st,
				   &ltc4282_in1_crit_fault_log);
	debugfs_create_file_unsafe("in1_lcrit_fault_log", 0400, dentry, st,
				   &ltc4282_in1_lcrit_fault_log);
	debugfs_create_file_unsafe("curr1_crit_fault_log", 0400, dentry, st,
				   &ltc4282_curr1_crit_fault_log);
}

static int ltc4282_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev, *hwmon;
	struct ltc4282_state *st;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate memory\n");

	st->map = devm_regmap_init_i2c(i2c, &ltc4282_regmap_config);
	if (IS_ERR(st->map))
		return dev_err_probe(dev, PTR_ERR(st->map),
				     "failed regmap init\n");

	/* Soft reset */
	ret = regmap_set_bits(st->map, LTC4282_ADC_CTRL, LTC4282_RESET_MASK);
	if (ret)
		return ret;

	/* Yes, it's big but it is as specified in the datasheet */
	msleep(3200);

	ret = ltc428_clks_setup(st, dev);
	if (ret)
		return ret;

	ret = ltc4282_setup(st, dev);
	if (ret)
		return ret;

	mutex_init(&st->lock);
	hwmon = devm_hwmon_device_register_with_info(dev, "ltc4282", st,
						     &ltc2947_chip_info,
						     ltc4282_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	ltc4282_debugfs_init(st, i2c, hwmon);

	return 0;
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
