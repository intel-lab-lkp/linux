// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Liebherr-Electronics and Drives GmbH
 *
 * Limitations:
 * - Supports frequencies between 0.5Hz and 2048Hz with following steps:
 *   - 0.5 Hz steps from 0.5 Hz to 32 Hz
 *   - 2 Hz steps from 2 Hz to 128 Hz
 *   - 8 Hz steps from 8 Hz to 512 Hz
 *   - 32 Hz steps from 32 Hz to 2048 Hz
 * - Cannot generate a 0 % duty cycle.
 * - Always produces low output if disabled.
 * - Configuration isn't atomic. When changing polarity, duty cycle or period
 *   the data is taken immediately, counters not being affected, resulting in a
 *   behavior of the output pin that is neither the old nor the new state,
 *   rather something in between.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pwm.h>

#include <asm/unaligned.h>

#include <linux/spi/spi.h>

#define MC33XS2410_GLB_CTRL		0x00
#define MC33XS2410_GLB_CTRL_MODE_MASK	GENMASK(7, 6)
#define MC33XS2410_GLB_CTRL_NORMAL_MODE	BIT(6)
#define MC33XS2410_PWM_CTRL1		0x05
#define MC33XS2410_PWM_CTRL1_POL_INV(x)	BIT(x)
#define MC33XS2410_PWM_CTRL3		0x07
/* x in { 0 ... 3 } */
#define MC33XS2410_PWM_CTRL3_EN(x)	BIT(4 + (x))
#define MC33XS2410_PWM_FREQ1		0x08
/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_FREQ(x)		(MC33XS2410_PWM_FREQ1 + (x - 1))
#define MC33XS2410_PWM_FREQ_STEP_MASK	GENMASK(7, 6)
#define MC33XS2410_PWM_FREQ_COUNT_MASK	GENMASK(5, 0)
#define MC33XS2410_PWM_DC1		0x0c
/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_DC(x)		(MC33XS2410_PWM_DC1 + (x - 1))
#define MC33XS2410_WDT			0x14

#define MC33XS2410_WR			BIT(7)
#define MC33XS2410_RD_CTRL		BIT(7)
#define MC33XS2410_RD_DATA_MASK		GENMASK(13, 0)

#define MC33XS2410_MIN_PERIOD_STEP0	31250000
#define MC33XS2410_MAX_PERIOD_STEP0	2000000000
/* x in { 0 ... 3 } */
#define MC33XS2410_MIN_PERIOD_STEP(x)	(MC33XS2410_MIN_PERIOD_STEP0 >> (2 * x))
/* x in { 0 ... 3 } */
#define MC33XS2410_MAX_PERIOD_STEP(x)	(MC33XS2410_MAX_PERIOD_STEP0 >> (2 * x))

#define MC33XS2410_MAX_TRANSFERS	5
#define MC33XS2410_WORD_LEN		2

struct mc33xs2410_pwm {
	struct spi_device *spi;
};

static
inline struct mc33xs2410_pwm *to_pwm_mc33xs2410_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int mc33xs2410_xfer_regs(struct spi_device *spi, bool read, u8 *reg,
				u16 *val, bool *ctrl, int len)
{
	struct spi_transfer t[MC33XS2410_MAX_TRANSFERS] = { { 0 } };
	u8 tx[MC33XS2410_MAX_TRANSFERS * MC33XS2410_WORD_LEN];
	u8 rx[MC33XS2410_MAX_TRANSFERS * MC33XS2410_WORD_LEN];
	int i, ret, reg_i, val_i;

	if (!len)
		return 0;

	if (read)
		len++;

	if (len > MC33XS2410_MAX_TRANSFERS)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		reg_i = i * MC33XS2410_WORD_LEN;
		val_i = reg_i + 1;
		if (read) {
			if (i < len - 1) {
				tx[reg_i] = reg[i];
				tx[val_i] = ctrl[i] ? MC33XS2410_RD_CTRL : 0;
				t[i].tx_buf = &tx[reg_i];
			}

			if (i > 0)
				t[i].rx_buf = &rx[reg_i - MC33XS2410_WORD_LEN];
		} else {
			tx[reg_i] = reg[i] | MC33XS2410_WR;
			tx[val_i] = val[i];
			t[i].tx_buf = &tx[reg_i];
		}

		t[i].len = MC33XS2410_WORD_LEN;
		t[i].cs_change = 1;
	}

	t[len - 1].cs_change = 0;

	ret = spi_sync_transfer(spi, &t[0], len);
	if (ret < 0)
		return ret;

	if (read) {
		for (i = 0; i < len - 1; i++) {
			reg_i = i * MC33XS2410_WORD_LEN;
			val[i] = FIELD_GET(MC33XS2410_RD_DATA_MASK,
					   get_unaligned_be16(&rx[reg_i]));
		}
	}

	return 0;
}

static
int mc33xs2410_write_regs(struct spi_device *spi, u8 *reg, u16 *val, int len)
{

	return mc33xs2410_xfer_regs(spi, false, reg, val, NULL, len);
}

static int mc33xs2410_read_regs(struct spi_device *spi, u8 *reg, bool *ctrl,
				u16 *val, u8 len)
{
	return mc33xs2410_xfer_regs(spi, true, reg, val, ctrl, len);
}


static int mc33xs2410_write_reg(struct spi_device *spi, u8 reg, u16 val)
{
	return mc33xs2410_write_regs(spi, &reg, &val, 1);
}

static
int mc33xs2410_read_reg(struct spi_device *spi, u8 reg, u16 *val, bool ctrl)
{
	return mc33xs2410_read_regs(spi, &reg, &ctrl, val, 1);
}

static int mc33xs2410_read_reg_ctrl(struct spi_device *spi, u8 reg, u16 *val)
{
	return mc33xs2410_read_reg(spi, reg, val, true);
}

static
int mc33xs2410_modify_reg(struct spi_device *spi, u8 reg, u16 mask, u16 val)
{
	u16 tmp;
	int ret;

	ret = mc33xs2410_read_reg_ctrl(spi, reg, &tmp);
	if (ret < 0)
		return ret;

	tmp &= ~mask;
	tmp |= val & mask;

	return mc33xs2410_write_reg(spi, reg, tmp);
}

static u8 mc33xs2410_pwm_get_freq(u64 period)
{
	u8 step, count;

	/*
	 * Check if period is within the limits of each of the four frequency
	 * ranges, starting with the highest frequency(lowest period). Higher
	 * frequencies are represented with better resolution by the device.
	 * Therefore favor frequency range with the better resolution to
	 * minimize error introduced by the frequency steps.
	 */

	switch (period) {
	case MC33XS2410_MIN_PERIOD_STEP(3) + 1 ... MC33XS2410_MAX_PERIOD_STEP(3):
		step = 3;
		break;
	case MC33XS2410_MAX_PERIOD_STEP(3) + 1 ... MC33XS2410_MAX_PERIOD_STEP(2):
		step = 2;
		break;
	case MC33XS2410_MAX_PERIOD_STEP(2) + 1 ... MC33XS2410_MAX_PERIOD_STEP(1):
		step = 1;
		break;
	case MC33XS2410_MAX_PERIOD_STEP(1) + 1 ... MC33XS2410_MAX_PERIOD_STEP(0):
		step = 0;
		break;
	}

	count = DIV_ROUND_UP((u32)MC33XS2410_MAX_PERIOD_STEP(step), (u32)period);
	return FIELD_PREP(MC33XS2410_PWM_FREQ_STEP_MASK, step) |
	       FIELD_PREP(MC33XS2410_PWM_FREQ_COUNT_MASK, count - 1);
}

static u64 mc33xs2410_pwm_get_period(u8 reg)
{
	u32 freq, code, steps;

	/*
	 * steps:
	 *   - 0 = 0.5Hz
	 *   - 1 = 2Hz
	 *   - 2 = 8Hz
	 *   - 3 = 32Hz
	 * frequency = (code + 1) x steps.
	 *
	 * To avoid division in case steps value is zero we scale the steps
	 * value for now by two and keep it in mind when calculating the period
	 * that we have doubled the frequency.
	 */
	steps = 1 << (FIELD_GET(MC33XS2410_PWM_FREQ_STEP_MASK, reg) * 2);
	code = FIELD_GET(MC33XS2410_PWM_FREQ_COUNT_MASK, reg);
	freq = (code + 1) * steps;

	/* Convert frequency to period, considering the doubled frequency. */
	return DIV_ROUND_UP((u32)(2 * NSEC_PER_SEC), freq);
}

static int mc33xs2410_pwm_get_relative_duty_cycle(u64 period, u64 duty_cycle)
{
	if (!period)
		return 0;

	duty_cycle *= 256;
	duty_cycle = DIV_ROUND_CLOSEST_ULL(duty_cycle, period);

	/* Device is not able to generate 0% duty cycle */
	if (!duty_cycle)
		return -ERANGE;

	return duty_cycle - 1;
}

static int mc33xs2410_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				const struct pwm_state *state)
{
	struct mc33xs2410_pwm *mc33xs2410 = to_pwm_mc33xs2410_chip(chip);
	struct spi_device *spi = mc33xs2410->spi;
	u8 reg[4] = {
			MC33XS2410_PWM_FREQ(pwm->hwpwm + 1),
			MC33XS2410_PWM_DC(pwm->hwpwm + 1),
			MC33XS2410_PWM_CTRL1,
			MC33XS2410_PWM_CTRL3
		    };
	bool ctrl[2] = { true, true };
	u64 period, duty_cycle;
	u16 val[4];
	u8 mask;
	int ret;

	period = min(state->period, MC33XS2410_MAX_PERIOD_STEP(0));
	if (period < MC33XS2410_MIN_PERIOD_STEP(3) + 1)
		return -EINVAL;

	ret = mc33xs2410_read_regs(spi, &reg[2], &ctrl[0], &val[2], 2);
	if (ret < 0)
		return ret;

	/* frequency */
	val[0] = mc33xs2410_pwm_get_freq(period);

	/* duty cycle */
	duty_cycle = min(period, state->duty_cycle);
	ret = mc33xs2410_pwm_get_relative_duty_cycle(period, duty_cycle);
	if (ret < 0)
		return ret;

	val[1] = ret;
	/* polarity */
	mask = MC33XS2410_PWM_CTRL1_POL_INV(pwm->hwpwm);
	val[2] = (state->polarity == PWM_POLARITY_INVERSED) ?
		 (val[2] | mask) : (val[2] & ~mask);

	/* enable output */
	mask = MC33XS2410_PWM_CTRL3_EN(pwm->hwpwm);
	val[3] = (state->enabled) ? (val[3] | mask) : (val[3] & ~mask);

	return mc33xs2410_write_regs(spi, reg, val, 4);
}

static int mc33xs2410_pwm_get_state(struct pwm_chip *chip,
				    struct pwm_device *pwm,
				    struct pwm_state *state)
{
	struct mc33xs2410_pwm *mc33xs2410 = to_pwm_mc33xs2410_chip(chip);
	struct spi_device *spi = mc33xs2410->spi;
	u8 reg[4] = {
			MC33XS2410_PWM_FREQ(pwm->hwpwm + 1),
			MC33XS2410_PWM_DC(pwm->hwpwm + 1),
			MC33XS2410_PWM_CTRL1,
			MC33XS2410_PWM_CTRL3,
		    };
	bool ctrl[4] = { true, true, true, true };
	u16 val[4];
	int ret;

	ret = mc33xs2410_read_regs(spi, reg, ctrl, val, 4);
	if (ret < 0)
		return ret;

	state->period = mc33xs2410_pwm_get_period(val[0]);
	pwm_set_relative_duty_cycle(state, val[1] + 1, 256);
	state->polarity = (val[2] & MC33XS2410_PWM_CTRL1_POL_INV(pwm->hwpwm)) ?
			  PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;

	state->enabled = !!(val[3] & MC33XS2410_PWM_CTRL3_EN(pwm->hwpwm));

	return 0;
}

static const struct pwm_ops mc33xs2410_pwm_ops = {
	.apply = mc33xs2410_pwm_apply,
	.get_state = mc33xs2410_pwm_get_state,
};

static int mc33xs2410_reset(struct device *dev)
{
	struct gpio_desc *reset_gpio;

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(reset_gpio))
		return PTR_ERR_OR_ZERO(reset_gpio);

	fsleep(1000);
	gpiod_set_value_cansleep(reset_gpio, 0);
	/* Wake-up time */
	fsleep(10000);

	return 0;
}

static int mc33xs2410_probe(struct spi_device *spi)
{
	struct mc33xs2410_pwm *mc33xs2410;
	struct device *dev = &spi->dev;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(dev, 4, sizeof(*mc33xs2410));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	mc33xs2410 = to_pwm_mc33xs2410_chip(chip);
	mc33xs2410->spi = spi;
	chip->ops = &mc33xs2410_pwm_ops;

	ret = mc33xs2410_reset(dev);
	if (ret)
		return ret;

	/* Disable watchdog */
	ret = mc33xs2410_write_reg(spi, MC33XS2410_WDT, 0x0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to disable watchdog\n");

	/* Transition to normal mode */
	ret = mc33xs2410_modify_reg(spi, MC33XS2410_GLB_CTRL,
				    MC33XS2410_GLB_CTRL_MODE_MASK,
				    MC33XS2410_GLB_CTRL_NORMAL_MODE);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to transition to normal mode\n");

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to add pwm chip\n");

	return 0;
}

static const struct spi_device_id mc33xs2410_spi_id[] = {
	{ "mc33xs2410" },
	{ }
};
MODULE_DEVICE_TABLE(spi, mc33xs2410_spi_id);

static const struct of_device_id mc33xs2410_of_match[] = {
	{ .compatible = "nxp,mc33xs2410" },
	{ }
};
MODULE_DEVICE_TABLE(of, mc33xs2410_of_match);

static struct spi_driver mc33xs2410_driver = {
	.driver = {
		.name = "mc33xs2410-pwm",
		.of_match_table = mc33xs2410_of_match,
	},
	.probe = mc33xs2410_probe,
	.id_table = mc33xs2410_spi_id,
};
module_spi_driver(mc33xs2410_driver);

MODULE_DESCRIPTION("NXP MC33XS2410 high-side switch driver");
MODULE_AUTHOR("Dimitri Fedrau <dima.fedrau@gmail.com>");
MODULE_LICENSE("GPL");
