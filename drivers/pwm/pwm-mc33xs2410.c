// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Liebherr-Electronics and Drives GmbH
 *
 * Reference Manual : https://www.nxp.com/docs/en/data-sheet/MC33XS2410.pdf
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

#include <linux/spi/spi.h>

#define MC33XS2410_GLB_CTRL		0x00
#define MC33XS2410_GLB_CTRL_MODE	GENMASK(7, 6)
#define MC33XS2410_GLB_CTRL_MODE_NORMAL	FIELD_PREP(MC33XS2410_GLB_CTRL_MODE, 1)

#define MC33XS2410_PWM_CTRL1		0x05
/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_CTRL1_POL_INV(x)	BIT((x - 1))

#define MC33XS2410_PWM_CTRL3		0x07
/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_CTRL3_EN(x)	BIT(4 + (x - 1))

/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_FREQ(x)		(0x08 + (x - 1))
#define MC33XS2410_PWM_FREQ_STEP	GENMASK(7, 6)
#define MC33XS2410_PWM_FREQ_COUNT	GENMASK(5, 0)

/* x in { 1 ... 4 } */
#define MC33XS2410_PWM_DC(x)		(0x0c + (x - 1))

#define MC33XS2410_WDT			0x14

#define MC33XS2410_PWM_MIN_PERIOD	488282
/* x in { 0 ... 3 } */
#define MC33XS2410_PWM_MAX_PERIOD(x)	(2000000000 >> (2 * x))

#define MC33XS2410_FRAME_IN_ADDR	GENMASK(15, 8)
#define MC33XS2410_FRAME_IN_DATA	GENMASK(7, 0)
#define MC33XS2410_FRAME_IN_ADDR_WR	BIT(7)
#define MC33XS2410_FRAME_IN_DATA_RD	BIT(7)
#define MC33XS2410_FRAME_OUT_DATA	GENMASK(13, 0)

#define MC33XS2410_MAX_TRANSFERS	5

struct mc33xs2410_pwm {
	struct spi_device *spi;
};

static inline struct mc33xs2410_pwm *mc33xs2410_from_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int mc33xs2410_write_regs(struct spi_device *spi, u8 *reg, u8 *val,
				 unsigned int len)
{
	u16 tx[MC33XS2410_MAX_TRANSFERS];
	int i;

	if (len > MC33XS2410_MAX_TRANSFERS)
		return -EINVAL;

	for (i = 0; i < len; i++)
		tx[i] = FIELD_PREP(MC33XS2410_FRAME_IN_DATA, val[i]) |
			FIELD_PREP(MC33XS2410_FRAME_IN_ADDR,
				   MC33XS2410_FRAME_IN_ADDR_WR | reg[i]);

	return spi_write(spi, tx, len * 2);
}

static int mc33xs2410_read_regs(struct spi_device *spi, u8 *reg, u8 flag,
				u16 *val, unsigned int len)
{
	u16 tx[MC33XS2410_MAX_TRANSFERS];
	u16 rx[MC33XS2410_MAX_TRANSFERS];
	struct spi_transfer t = {
		.tx_buf = tx,
		.rx_buf = rx,
	};
	int i, ret;

	len++;
	if (len > MC33XS2410_MAX_TRANSFERS)
		return -EINVAL;

	t.len = len * 2;
	for (i = 0; i < len - 1; i++)
		tx[i] = FIELD_PREP(MC33XS2410_FRAME_IN_DATA, flag) |
			FIELD_PREP(MC33XS2410_FRAME_IN_ADDR, reg[i]);

	ret = spi_sync_transfer(spi, &t, 1);
	if (ret < 0)
		return ret;

	for (i = 1; i < len; i++)
		val[i - 1] = FIELD_GET(MC33XS2410_FRAME_OUT_DATA, rx[i]);

	return 0;
}


static int mc33xs2410_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	return mc33xs2410_write_regs(spi, &reg, &val, 1);
}

static int mc33xs2410_read_reg(struct spi_device *spi, u8 reg, u16 *val, u8 flag)
{
	return mc33xs2410_read_regs(spi, &reg, flag, val, 1);
}

static int mc33xs2410_read_reg_ctrl(struct spi_device *spi, u8 reg, u16 *val)
{
	return mc33xs2410_read_reg(spi, reg, val, MC33XS2410_FRAME_IN_DATA_RD);
}

static int mc33xs2410_modify_reg(struct spi_device *spi, u8 reg, u8 mask, u8 val)
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
	 * Check which step is appropriate for the given period, starting with
	 * the highest frequency(lowest period). Higher frequencies are
	 * represented with better resolution by the device. Therefore favor
	 * frequency range with the better resolution to minimize error
	 * introduced by the frequency steps.
	 */

	switch (period) {
	case MC33XS2410_PWM_MIN_PERIOD ... MC33XS2410_PWM_MAX_PERIOD(3):
		step = 3;
		break;
	case MC33XS2410_PWM_MAX_PERIOD(3) + 1 ... MC33XS2410_PWM_MAX_PERIOD(2):
		step = 2;
		break;
	case MC33XS2410_PWM_MAX_PERIOD(2) + 1 ... MC33XS2410_PWM_MAX_PERIOD(1):
		step = 1;
		break;
	case MC33XS2410_PWM_MAX_PERIOD(1) + 1 ... MC33XS2410_PWM_MAX_PERIOD(0):
		step = 0;
		break;
	}

	/*
	 * Round up here because a higher count results in a higher frequency
	 * and so a smaller period.
	 */
	count = DIV_ROUND_UP((u32)MC33XS2410_PWM_MAX_PERIOD(step), (u32)period);
	return FIELD_PREP(MC33XS2410_PWM_FREQ_STEP, step) |
	       FIELD_PREP(MC33XS2410_PWM_FREQ_COUNT, count - 1);
}

static u64 mc33xs2410_pwm_get_period(u8 reg)
{
	u32 freq, code, doubled_steps;

	/*
	 * steps:
	 *   - 0 = 0.5Hz
	 *   - 1 = 2Hz
	 *   - 2 = 8Hz
	 *   - 3 = 32Hz
	 * frequency = (code + 1) x steps.
	 *
	 * To avoid losing precision in case steps value is zero, scale the
	 * steps value for now by two and keep it in mind when calculating the
	 * period that the frequency had been doubled.
	 */
	doubled_steps = 1 << (FIELD_GET(MC33XS2410_PWM_FREQ_STEP, reg) * 2);
	code = FIELD_GET(MC33XS2410_PWM_FREQ_COUNT, reg);
	freq = (code + 1) * doubled_steps;

	/* Convert frequency to period, considering the doubled frequency. */
	return DIV_ROUND_UP(2 * NSEC_PER_SEC, freq);
}

static int mc33xs2410_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				const struct pwm_state *state)
{
	struct mc33xs2410_pwm *mc33xs2410 = mc33xs2410_from_chip(chip);
	struct spi_device *spi = mc33xs2410->spi;
	u8 reg[4] = {
			MC33XS2410_PWM_FREQ(pwm->hwpwm + 1),
			MC33XS2410_PWM_DC(pwm->hwpwm + 1),
			MC33XS2410_PWM_CTRL1,
			MC33XS2410_PWM_CTRL3
		    };
	u64 period, duty_cycle;
	int ret, rel_dc;
	u16 rd_val[2];
	u8 wr_val[4];
	u8 mask;

	period = min(state->period, MC33XS2410_PWM_MAX_PERIOD(0));
	if (period < MC33XS2410_PWM_MIN_PERIOD)
		return -EINVAL;

	ret = mc33xs2410_read_regs(spi, &reg[2], MC33XS2410_FRAME_IN_DATA_RD, rd_val, 2);
	if (ret < 0)
		return ret;

	/* frequency */
	wr_val[0] = mc33xs2410_pwm_get_freq(period);
	/* Continue calculations with the possibly truncated period */
	period = mc33xs2410_pwm_get_period(wr_val[0]);

	/* duty cycle */
	duty_cycle = min(period, state->duty_cycle);
	rel_dc = div64_u64(duty_cycle * 256, period) - 1;
	if (rel_dc < 0)
		wr_val[1] = 0;
	else
		wr_val[1] = rel_dc;

	/* polarity */
	mask = MC33XS2410_PWM_CTRL1_POL_INV(pwm->hwpwm + 1);
	wr_val[2] = (state->polarity == PWM_POLARITY_INVERSED) ?
		    (rd_val[0] | mask) : (rd_val[0] & ~mask);

	/* enable output */
	mask = MC33XS2410_PWM_CTRL3_EN(pwm->hwpwm + 1);
	wr_val[3] = (state->enabled && rel_dc >= 0) ? (rd_val[1] | mask) :
						      (rd_val[1] & ~mask);

	return mc33xs2410_write_regs(spi, reg, wr_val, 4);
}

static int mc33xs2410_pwm_get_state(struct pwm_chip *chip,
				    struct pwm_device *pwm,
				    struct pwm_state *state)
{
	struct mc33xs2410_pwm *mc33xs2410 = mc33xs2410_from_chip(chip);
	struct spi_device *spi = mc33xs2410->spi;
	u8 reg[4] = {
			MC33XS2410_PWM_FREQ(pwm->hwpwm + 1),
			MC33XS2410_PWM_DC(pwm->hwpwm + 1),
			MC33XS2410_PWM_CTRL1,
			MC33XS2410_PWM_CTRL3,
		    };
	u16 val[4];
	int ret;

	ret = mc33xs2410_read_regs(spi, reg, MC33XS2410_FRAME_IN_DATA_RD, val, 4);
	if (ret < 0)
		return ret;

	state->period = mc33xs2410_pwm_get_period(val[0]);
	state->polarity = (val[2] & MC33XS2410_PWM_CTRL1_POL_INV(pwm->hwpwm + 1)) ?
			  PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;
	state->enabled = !!(val[3] & MC33XS2410_PWM_CTRL3_EN(pwm->hwpwm + 1));
	state->duty_cycle = DIV_ROUND_UP_ULL((val[1] + 1) * state->period, 256);

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

	spi->bits_per_word = 16;
	spi->mode |= SPI_CS_WORD;
	ret = spi_setup(spi);
	if (ret < 0)
		return ret;

	mc33xs2410 = mc33xs2410_from_chip(chip);
	mc33xs2410->spi = spi;
	chip->ops = &mc33xs2410_pwm_ops;

	ret = mc33xs2410_reset(dev);
	if (ret)
		return ret;

	/*
	 * Disable watchdog and keep in mind that the watchdog won't trigger a
	 * reset of the machine when running into an timeout, instead the
	 * control over the outputs is handed over to the INx input logic
	 * signals of the device. Disabling it here just deactivates this
	 * feature until a proper solution is found.
	 */
	ret = mc33xs2410_write_reg(spi, MC33XS2410_WDT, 0x0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to disable watchdog\n");

	/* Transition to normal mode */
	ret = mc33xs2410_modify_reg(spi, MC33XS2410_GLB_CTRL,
				    MC33XS2410_GLB_CTRL_MODE,
				    MC33XS2410_GLB_CTRL_MODE_NORMAL);
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
