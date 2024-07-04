// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * g762 - Driver for the Global Mixed-mode Technology Inc. fan speed
 *        PWM controller chips from G762 family, i.e. G762 and G763
 *
 * Copyright (C) 2013, Arnaud EBALARD <arno@natisbad.org>
 *
 * This work is based on a basic version for 2.6.31 kernel developed
 * by Olivier Mouchet for LaCie. Updates and correction have been
 * performed to run on recent kernels. Additional features, like the
 * ability to configure various characteristics via .dts file or
 * board init file have been added. Detailed datasheet on which this
 * development is based is available here:
 *
 *  http://natisbad.org/NAS/refs/GMT_EDS-762_763-080710-0.2.pdf
 *
 * Headers from previous developments have been kept below:
 *
 * Copyright (c) 2009 LaCie
 *
 * Author: Olivier Mouchet <olivier.mouchet@gmail.com>
 *
 * based on g760a code written by Herbert Valerio Riedel <hvr@gnu.org>
 * Copyright (C) 2007  Herbert Valerio Riedel <hvr@gnu.org>
 *
 * g762: minimal datasheet available at:
 *       http://www.gmt.com.tw/product/datasheet/EDS-762_3.pdf
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define DRVNAME "g762"

enum g762_regs {
	G762_REG_SET_CNT  = 0x00,
	G762_REG_ACT_CNT  = 0x01,
	G762_REG_FAN_STA  = 0x02,
	G762_REG_SET_OUT  = 0x03,
	G762_REG_FAN_CMD1 = 0x04,
	G762_REG_FAN_CMD2 = 0x05,
};

/* Config register bits */
#define G762_REG_FAN_CMD1_DET_FAN_FAIL	BIT(7)	/* enable fan_fail signal */
#define G762_REG_FAN_CMD1_DET_FAN_OOC	BIT(6)	/* enable fan_out_of_control */
#define G762_REG_FAN_CMD1_OUT_MODE	BIT(5)	/* out mode: PWM or DC */
#define G762_REG_FAN_CMD1_FAN_MODE	BIT(4)	/* fan mode: closed/open-loop */
#define G762_REG_FAN_CMD1_CLK_DIV_MASK	GENMASK(3, 2)
#define G762_REG_FAN_CMD1_PWM_POLARITY	BIT(1)	/* PWM polarity */
#define G762_REG_FAN_CMD1_PULSE_PER_REV	BIT(0)	/* pulse per fan revolution */

#define G761_REG_FAN_CMD2_FAN_CLOCK	BIT(5)	/* choose internal clock*/
#define G762_REG_FAN_CMD2_GEAR_MASK	GENMASK(3, 2)
#define G762_REG_FAN_CMD2_FAN_STARTV_MASK GENMASK(1, 0)	/* fan startup voltage */

#define G762_REG_FAN_STA_FAIL		BIT(1)	/* fan fail */
#define G762_REG_FAN_STA_OOC		BIT(0)	/* fan out of control */

/*
 * Extract pulse count per fan revolution value (2 or 4) from given
 * FAN_CMD1 register value.
 */
#define G762_PULSE_FROM_REG(reg) \
	((((reg) & G762_REG_FAN_CMD1_PULSE_PER_REV) + 1) << 1)

/*
 * Extract fan clock divisor (1, 2, 4 or 8) from given FAN_CMD1
 * register value.
 */
#define G762_CLKDIV_FROM_REG(reg) \
	BIT(FIELD_GET(G762_REG_FAN_CMD1_CLK_DIV_MASK, reg))

/*
 * Extract fan gear mode multiplier value (1, 2 or 4) from given
 * FAN_CMD2 register value.
 */
#define G762_GEARMULT_FROM_REG(reg) \
	BIT(FIELD_GET(G762_REG_FAN_CMD2_GEAR_MASK, reg))

struct g762_data {
	struct regmap *regmap;

	/* update mutex */
	struct mutex update_lock;

	/* board specific parameters. */
	u32 clk_freq;
};

/*
 * Convert count value from fan controller register (FAN_SET_CNT) into fan
 * speed RPM value. Note that the datasheet documents a basic formula;
 * influence of additional parameters (fan clock divisor, fan gear mode)
 * have been infered from examples in the datasheet and tests.
 */
static inline unsigned int rpm_from_cnt(u8 cnt, u32 clk_freq, u16 p,
					u8 clk_div, u8 gear_mult)
{
	if (cnt == 0xff)  /* setting cnt to 255 stops the fan */
		return 0;

	return (clk_freq * 30 * gear_mult) / ((cnt ? cnt : 1) * p * clk_div);
}

/*
 * Convert fan RPM value into count value for fan controller
 * register (FAN_SET_CNT).
 */
static inline unsigned char cnt_from_rpm(unsigned long rpm, u32 clk_freq, u16 p,
					 u8 clk_div, u8 gear_mult)
{
	unsigned long f1 = clk_freq * 30 * gear_mult;
	unsigned long f2 = p * clk_div;

	if (!rpm)	/* to stop the fan, set cnt to 255 */
		return 0xff;

	rpm = clamp_val(rpm, f1 / (255 * f2), ULONG_MAX / f2);
	return DIV_ROUND_CLOSEST(f1, rpm * f2);
}

/* helpers for writing hardware parameters */

/*
 * Set input clock frequency received on CLK pin of the chip. Accepted values
 * are between 0 and 0xffffff. If zero is given, then default frequency
 * (32,768Hz) is used. Note that clock frequency is a characteristic of the
 * system but an internal parameter, i.e. value is not passed to the device.
 */
static int do_set_clk_freq(struct device *dev, unsigned long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val > 0xffffff)
		return -EINVAL;
	if (!val)
		val = 32768;

	data->clk_freq = val;

	return 0;
}

/* Set pwm mode. Accepts either 0 (PWM mode) or 1 (DC mode) */
static int do_set_pwm_mode(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val < 0 || val > 1)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD1,
				  G762_REG_FAN_CMD1_OUT_MODE,
				  val ? G762_REG_FAN_CMD1_OUT_MODE : 0);
}

/* Set fan clock divisor. Accepts either 1, 2, 4 or 8. */
static int do_set_fan_div(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val < 0 || hweight_long(val) != 1 || val > 8)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD1,
				  G762_REG_FAN_CMD1_CLK_DIV_MASK,
				  FIELD_PREP(G762_REG_FAN_CMD1_CLK_DIV_MASK, __ffs(val)));
}

/* Set fan gear mode. Accepts either 0, 1 or 2. */
static int do_set_fan_gear_mode(struct device *dev, u32 val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val > 2)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD2,
				  G762_REG_FAN_CMD2_GEAR_MASK,
				  FIELD_PREP(G762_REG_FAN_CMD2_GEAR_MASK, val));
}

/* Set number of fan pulses per revolution. Accepts either 2 or 4. */
static int do_set_fan_pulses(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val != 2 && val != 4)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD1,
				  G762_REG_FAN_CMD1_PULSE_PER_REV,
				  val == 4 ? G762_REG_FAN_CMD1_PULSE_PER_REV : 0);
}

/* Set fan mode. Accepts either 1 (open-loop) or 2 (closed-loop). */
static int do_set_pwm_enable(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	int ret;

	if (val != 1 && val != 2)
		return -EINVAL;

	mutex_lock(&data->update_lock);
	if (val == 1) {
		u32 regval;

		ret = regmap_read(regmap, G762_REG_SET_CNT, &regval);
		if (ret)
			goto unlock;
		/*
		 * BUG FIX: if SET_CNT register value is 255 then, for some
		 * unknown reason, fan will not rotate as expected, no matter
		 * the value of SET_OUT (to be specific, this seems to happen
		 * only in PWM mode). To workaround this bug, we give SET_CNT
		 * value of 254 if it is 255 when switching to open-loop.
		 */
		if (regval == 0xff)
			regmap_write(regmap, G762_REG_SET_CNT, 254);
	}
	ret = regmap_update_bits(regmap, G762_REG_FAN_CMD1, G762_REG_FAN_CMD1_FAN_MODE,
				 val == 2 ? G762_REG_FAN_CMD1_FAN_MODE : 0);
unlock:
	mutex_unlock(&data->update_lock);
	return ret;
}

/* Set PWM polarity. Accepts either 0 (positive duty) or 1 (negative duty) */
static int do_set_pwm_polarity(struct device *dev, unsigned long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val && val != 1)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD1, G762_REG_FAN_CMD1_PWM_POLARITY,
				  val ? G762_REG_FAN_CMD1_PWM_POLARITY : 0);
}

/*
 * Set pwm value. Accepts values between 0 (stops the fan) and
 * 255 (full speed). This only makes sense in open-loop mode.
 */
static int do_set_pwm(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val < 0 || val > 255)
		return -EINVAL;

	return regmap_write(data->regmap, G762_REG_SET_OUT, val);
}

/*
 * Set fan RPM value. Can be called both in closed and open-loop mode
 * but effect will only be seen after closed-loop mode is configured.
 */
static int do_set_fan_target(struct device *dev, long val)
{
	struct g762_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	u32 cmd1, cmd2;
	u8 set_cnt;
	int ret;

	if (val < 0)
		return -EINVAL;

	mutex_lock(&data->update_lock);

	ret = regmap_read(regmap, G762_REG_FAN_CMD1, &cmd1);
	if (ret)
		goto unlock;
	ret = regmap_read(regmap, G762_REG_FAN_CMD2, &cmd2);
	if (ret)
		goto unlock;

	set_cnt = cnt_from_rpm(val, data->clk_freq,
			       G762_PULSE_FROM_REG(cmd1),
			       G762_CLKDIV_FROM_REG(cmd1),
			       G762_GEARMULT_FROM_REG(cmd2));
	ret = regmap_write(regmap, G762_REG_SET_CNT, set_cnt);
unlock:
	mutex_unlock(&data->update_lock);
	return ret;
}

/* Set fan startup voltage. Accepted values are either 0, 1, 2 or 3. */
static int do_set_fan_startv(struct device *dev, unsigned long val)
{
	struct g762_data *data = dev_get_drvdata(dev);

	if (val > 3)
		return -EINVAL;

	return regmap_update_bits(data->regmap, G762_REG_FAN_CMD2,
				  G762_REG_FAN_CMD2_FAN_STARTV_MASK,
				  FIELD_PREP(G762_REG_FAN_CMD2_FAN_STARTV_MASK, val));
}

/* hwmon API access and helper functions */

static int get_fan_rpm_locked(struct g762_data *data, int reg)
{
	struct regmap *regmap = data->regmap;
	u32 cmd1, cmd2, count;
	int ret;

	ret = regmap_read(regmap, reg, &count);
	if (ret)
		return ret;
	ret = regmap_read(regmap, G762_REG_FAN_CMD1, &cmd1);
	if (ret)
		return ret;
	ret = regmap_read(regmap, G762_REG_FAN_CMD2, &cmd2);
	if (ret)
		return ret;

	return rpm_from_cnt(count, data->clk_freq,
			    G762_PULSE_FROM_REG(cmd1),
			    G762_CLKDIV_FROM_REG(cmd1),
			    G762_GEARMULT_FROM_REG(cmd2));
}

static int get_fan_rpm(struct g762_data *data, int reg)
{
	int ret;

	mutex_lock(&data->update_lock);
	ret = get_fan_rpm_locked(data, reg);
	mutex_unlock(&data->update_lock);

	return ret;
}

static int g762_fan_read(struct device *dev, u32 attr, long *val)
{
	struct g762_data *data = dev_get_drvdata(dev);
	u32 regval;
	int ret;

	switch (attr) {
	case hwmon_fan_target:
		ret = get_fan_rpm(data, G762_REG_SET_CNT);
		if (ret < 0)
			return ret;
		*val = ret;
		break;
	case hwmon_fan_div:
		ret = regmap_read(data->regmap, G762_REG_FAN_CMD1, &regval);
		if (ret < 0)
			return ret;
		*val = G762_CLKDIV_FROM_REG(regval);
		break;
	case hwmon_fan_pulses:
		ret = regmap_read(data->regmap, G762_REG_FAN_CMD1, &regval);
		if (ret < 0)
			return ret;
		*val = G762_PULSE_FROM_REG(regval);
		break;
	case hwmon_fan_input:
		mutex_lock(&data->update_lock);
		ret = regmap_read(data->regmap, G762_REG_FAN_STA, &regval);
		if (ret || !(regval & G762_REG_FAN_STA_OOC)) {
			mutex_unlock(&data->update_lock);
			return ret ? : -ENODATA;
		}
		ret = get_fan_rpm_locked(data, G762_REG_ACT_CNT);
		mutex_unlock(&data->update_lock);
		if (ret < 0)
			return ret;
		*val = ret;
		break;
	case hwmon_fan_alarm:
		ret = regmap_read(data->regmap, G762_REG_FAN_STA, &regval);
		if (ret < 0)
			return ret;
		/* G762_REG_FAN_STA_OOC is active low */
		*val = !(regval & G762_REG_FAN_STA_OOC);
		break;
	case hwmon_fan_fault:
		ret = regmap_read(data->regmap, G762_REG_FAN_STA, &regval);
		if (ret < 0)
			return ret;
		*val = !!(regval & G762_REG_FAN_STA_FAIL);
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int g762_pwm_read(struct device *dev, u32 attr, long *val)
{
	struct g762_data *data = dev_get_drvdata(dev);
	u32 regval;
	int ret;

	switch (attr) {
	case hwmon_pwm_input:
		ret = regmap_read(data->regmap, G762_REG_SET_OUT, &regval);
		if (ret < 0)
			return ret;
		*val = regval;
		break;
	case hwmon_pwm_mode:
		ret = regmap_read(data->regmap, G762_REG_FAN_CMD1, &regval);
		if (ret < 0)
			return ret;
		*val = !!(regval & G762_REG_FAN_CMD1_OUT_MODE);
		break;
	case hwmon_pwm_enable:
		ret = regmap_read(data->regmap, G762_REG_FAN_CMD1, &regval);
		if (ret < 0)
			return ret;
		*val = !!(regval & G762_REG_FAN_CMD1_FAN_MODE) + 1;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int g762_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_fan:
		return g762_fan_read(dev, attr, val);
	case hwmon_pwm:
		return g762_pwm_read(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int g762_fan_write(struct device *dev, u32 attr, long val)
{
	switch (attr) {
	case hwmon_fan_target:
		return do_set_fan_target(dev, val);
	case hwmon_fan_div:
		return do_set_fan_div(dev, val);
	case hwmon_fan_pulses:
		return do_set_fan_pulses(dev, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int g762_pwm_write(struct device *dev, u32 attr, long val)
{
	switch (attr) {
	case hwmon_pwm_input:
		return do_set_pwm(dev, val);
	case hwmon_pwm_mode:
		return do_set_pwm_mode(dev, val);
	case hwmon_pwm_enable:
		return do_set_pwm_enable(dev, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int g762_write(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_fan:
		return g762_fan_write(dev, attr, val);
	case hwmon_pwm:
		return g762_pwm_write(dev, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t g762_is_visible(const void *_data, enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_alarm:
		case hwmon_fan_fault:
			return 0444;
		case hwmon_fan_target:
		case hwmon_fan_div:
		case hwmon_fan_pulses:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_mode:
		case hwmon_pwm_enable:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static const struct hwmon_channel_info * const g762_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_FAULT |
			   HWMON_F_TARGET | HWMON_F_DIV | HWMON_F_PULSES),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops g762_hwmon_ops = {
	.is_visible = g762_is_visible,
	.read = g762_read,
	.write = g762_write,
};

static const struct hwmon_chip_info g762_chip_info = {
	.ops = &g762_hwmon_ops,
	.info = g762_info,
};

/*
 * Enable both fan failure detection and fan out of control protection.
 */
static inline int g762_fan_init(struct device *dev, bool internal)
{
	struct g762_data *data = dev_get_drvdata(dev);
	int ret;

	ret = regmap_set_bits(data->regmap, G762_REG_FAN_CMD1,
			      G762_REG_FAN_CMD1_DET_FAN_FAIL | G762_REG_FAN_CMD1_DET_FAN_OOC);
	if (ret)
		return ret;

	/* internal_clock can only be set with compatible g761 */
	if (internal)
		return regmap_set_bits(data->regmap, G762_REG_FAN_CMD2,
				       G761_REG_FAN_CMD2_FAN_CLOCK);
	return 0;
}

/*
 * Grab clock (a required property), enable it, get (fixed) clock frequency
 * and store it.
 */
static int g762_clock_enable(struct device *dev, bool *internal)
{
	unsigned long clk_freq = 32768;
	struct clk *clk;
	bool _internal;
	int ret;

	_internal = device_is_compatible(dev, "gmt,g761") &&
			       !device_property_present(dev, "clocks");
	if (!_internal) {
		clk = devm_clk_get_enabled(dev, NULL);
		if (IS_ERR(clk)) {
			if (dev_fwnode(dev))
				return dev_err_probe(dev, PTR_ERR(clk), "failed to enable clock\n");
		} else {
			clk_freq = clk_get_rate(clk);
		}
	}

	ret = do_set_clk_freq(dev, clk_freq);
	if (ret)
		return dev_err_probe(dev, ret, "invalid clock freq %lu\n", clk_freq);

	*internal = _internal;
	return 0;
}

static int g762_configure(struct device *dev)
{
	bool internal;
	u32 property;
	int ret;

	ret = g762_clock_enable(dev, &internal);
	if (ret)
		return ret;

	/* Enable fan failure detection and fan out of control protection */
	ret = g762_fan_init(dev, internal);
	if (ret)
		return ret;

	if (!device_property_read_u32(dev, "fan_gear_mode", &property)) {
		ret = do_set_fan_gear_mode(dev, property);
		if (ret)
			return ret;
	}

	if (!device_property_read_u32(dev, "pwm_polarity", &property)) {
		ret = do_set_pwm_polarity(dev, property);
		if (ret)
			return ret;
	}

	if (!device_property_read_u32(dev, "fan_startv", &property)) {
		ret = do_set_fan_startv(dev, property);
		if (ret)
			return ret;
	}
	return 0;
}

static bool g762_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg == G762_REG_SET_CNT || reg == G762_REG_SET_OUT ||
	  reg == G762_REG_FAN_CMD1 || reg == G762_REG_FAN_CMD2;
}

static bool g762_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == G762_REG_ACT_CNT || reg == G762_REG_FAN_STA;
}

static const struct regmap_config g762_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = G762_REG_FAN_CMD2,
	.writeable_reg = g762_writeable_reg,
	.volatile_reg = g762_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static int g762_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct g762_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(struct g762_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = devm_regmap_init_i2c(client, &g762_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	dev_set_drvdata(dev, data);
	mutex_init(&data->update_lock);

	ret = g762_configure(dev);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data, &g762_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id g762_dt_match[] = {
	{ .compatible = "gmt,g761" },
	{ .compatible = "gmt,g762" },
	{ .compatible = "gmt,g763" },
	{ },
};
MODULE_DEVICE_TABLE(of, g762_dt_match);

static const struct i2c_device_id g762_id[] = {
	{ "g761" },
	{ "g762" },
	{ "g763" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, g762_id);

static struct i2c_driver g762_driver = {
	.driver = {
		.name = DRVNAME,
		.of_match_table = g762_dt_match,
	},
	.probe = g762_probe,
	.id_table = g762_id,
};

module_i2c_driver(g762_driver);

MODULE_AUTHOR("Arnaud EBALARD <arno@natisbad.org>");
MODULE_DESCRIPTION("GMT G762/G763 driver");
MODULE_LICENSE("GPL");
