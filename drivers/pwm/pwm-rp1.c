// SPDX-License-Identifier: GPL-2.0
/*
 * pwm-rp1.c
 *
 * Raspberry Pi RP1 PWM.
 *
 * Copyright © 2026 Raspberry Pi Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 * Based on the pwm-bcm2835 driver by:
 * Bart Tanghe <bart.tanghe@thomasmore.be>
 *
 * Datasheet: https://pip-assets.raspberrypi.com/categories/892-raspberry-pi-5/documents/RP-008370-DS-1-rp1-peripherals.pdf?disposition=inline
 *
 * Limitations:
 * - Channels can be enabled/disabled through a global update flag, while the
 *   period and duty per-channel registers are independently updatable, and
 *   they are latched on the end of (specific channel) period strobe.
 *   This means that period and duty changes might result in glitches if the
 *   period/duty is changed exactly during an end of period strobe.
 * - Since the duty/period registers are freely updatable (do not depend on
 *   the global update flag), setting one of them close to the period end and
 *   the other right afterwards results in a mixed output for that cycle because
 *   the write ops are not atomic.
 * - The global update flag prevents mis-sampling of multi-bit bus signals in
 *   the PWM clock domain. This ensures that all PWM channel settings update
 *   on the same PWM clock cycle. Channels start in sync only if they share the
 *   same period.
 * - If both duty and period are set to 0, the output is a constant low signal
 *   if polarity is normal or a constant high signal if polarity is inversed.
 * - When disabled the output is driven to 0 if polarity is normal, or to 1
 *   if polarity is inversed.
 * - Disabling the PWM stops the output immediately, without waiting for current
 *   period to complete first.
 * - Channels are phase-capable, but on RPi5, the firmware can use a channel
 *   phase register to report the RPM of the fan connected to that PWM
 *   channel. As a result, phase control will be ignored for now.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/units.h>

#define RP1_PWM_GLB_CTRL			0x000
#define RP1_PWM_GLB_CTRL_CHANNEL_ENABLE(chan)	BIT(chan)
#define RP1_PWM_GLB_CTRL_SET_UPDATE		BIT(31)

#define RP1_PWM_CHAN_CTRL(chan)			(0x014 + ((chan) * 0x10))
#define RP1_PWM_CHAN_CTRL_POLARITY		BIT(3)
#define RP1_PWM_CHAN_CTRL_FIFO_POP_MASK		BIT(8)
#define RP1_PWM_CHAN_CTRL_MODE			GENMASK(2, 0)
enum rp1_pwm_ctrl_mode {
	RP1_PWM_CHAN_CTRL_MODE_ZERO,
	RP1_PWM_CHAN_CTRL_MODE_TE_MS,
	RP1_PWM_CHAN_CTRL_MODE_PC_MS,
	RP1_PWM_CHAN_CTRL_MODE_PD_ENC,
	RP1_PWM_CHAN_CTRL_MODE_MSB_SER,
	RP1_PWM_CHAN_CTRL_MODE_PPM,
	RP1_PWM_CHAN_CTRL_MODE_LE_MS,
	RP1_PWM_CHAN_CTRL_MODE_LSB_SER,
};

#define RP1_PWM_CHAN_CTRL_DEFAULT		(RP1_PWM_CHAN_CTRL_FIFO_POP_MASK +  \
						FIELD_PREP(RP1_PWM_CHAN_CTRL_MODE, \
						RP1_PWM_CHAN_CTRL_MODE_TE_MS))

#define RP1_PWM_RANGE(chan)			(0x018 + ((chan) * 0x10))
#define RP1_PWM_PHASE(chan)			(0x01C + ((chan) * 0x10))
#define RP1_PWM_DUTY(chan)			(0x020 + ((chan) * 0x10))

#define RP1_PWM_NUM_PWMS			4

struct rp1_pwm {
	struct regmap *regmap;
	struct clk *clk;
	unsigned long clk_rate;
	bool clk_enabled;
};

struct rp1_pwm_waveform {
	u32 period_ticks;
	u32 duty_ticks;
	bool enabled;
	bool inverted_polarity;
};

static const struct regmap_config rp1_pwm_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x60,
};

static void rp1_pwm_apply_config(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	u32 value;

	/* update the changed registers on the next strobe to avoid glitches */
	regmap_read(rp1->regmap, RP1_PWM_GLB_CTRL, &value);
	value |= RP1_PWM_GLB_CTRL_SET_UPDATE;
	regmap_write(rp1->regmap, RP1_PWM_GLB_CTRL, value);
}

static int rp1_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);

	/* init channel to reset defaults, preserving the polarity bit */
	regmap_update_bits(rp1->regmap, RP1_PWM_CHAN_CTRL(pwm->hwpwm),
			   ~(u32)RP1_PWM_CHAN_CTRL_POLARITY, RP1_PWM_CHAN_CTRL_DEFAULT);
	return 0;
}

static int rp1_pwm_round_waveform_tohw(struct pwm_chip *chip,
				       struct pwm_device *pwm,
				       const struct pwm_waveform *wf,
				       void *_wfhw)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	u64 period_ticks, duty_ticks, offset_ticks;
	struct rp1_pwm_waveform *wfhw = _wfhw;
	u64 clk_rate = rp1->clk_rate;
	int ret = 0;

	if (!wf->period_length_ns) {
		wfhw->enabled = false;
		wfhw->inverted_polarity = (pwm_get_polarity(pwm) == PWM_POLARITY_INVERSED);
		return 0;
	}

	period_ticks = mul_u64_u64_div_u64(wf->period_length_ns, clk_rate, NSEC_PER_SEC);

	/*
	 * The period is limited to U32_MAX, and it will be decremented by one later
	 * to allow 100% duty cycle.
	 */
	if (period_ticks > U32_MAX) {
		period_ticks = U32_MAX;
	} else if (period_ticks < 2) {
		period_ticks = 2;
		ret = 1;
	}

	duty_ticks = mul_u64_u64_div_u64(wf->duty_length_ns, clk_rate, NSEC_PER_SEC);
	duty_ticks = min(duty_ticks, period_ticks);
	offset_ticks = mul_u64_u64_div_u64(wf->duty_offset_ns, clk_rate, NSEC_PER_SEC);
	if (offset_ticks >= period_ticks) {
		u64 remainder;

		div64_u64_rem(offset_ticks, period_ticks, &remainder);
		offset_ticks = remainder;
	}
	if (duty_ticks && offset_ticks &&
	    duty_ticks + offset_ticks >= period_ticks) {
		wfhw->duty_ticks = period_ticks - duty_ticks;
		wfhw->inverted_polarity = true;
	} else {
		wfhw->duty_ticks = duty_ticks;
		wfhw->inverted_polarity = false;
	}
	/* Account for the extra tick at the end of the period */
	wfhw->period_ticks = period_ticks - 1;

	wfhw->enabled = true;

	return ret;
}

static int rp1_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const void *_wfhw,
					 struct pwm_waveform *wf)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	const struct rp1_pwm_waveform *wfhw = _wfhw;
	u64 clk_rate = rp1->clk_rate;
	u64 ticks;

	*wf = (struct pwm_waveform){ };

	if (!wfhw->enabled)
		return 0;

	wf->period_length_ns = DIV_ROUND_UP_ULL(((u64)wfhw->period_ticks + 1) * NSEC_PER_SEC,
						clk_rate);

	if (!wfhw->inverted_polarity) {
		wf->duty_length_ns = DIV_ROUND_UP_ULL((u64)wfhw->duty_ticks * NSEC_PER_SEC,
						      (u32)clk_rate);
	} else {
		if (wfhw->duty_ticks > (u64)wfhw->period_ticks + 1) {
			/* 100% duty cycle case */
			ticks = 0;
		} else {
			ticks = (u64)wfhw->period_ticks + 1 - wfhw->duty_ticks;
		}
		wf->duty_length_ns = DIV_ROUND_UP_ULL(ticks * NSEC_PER_SEC, clk_rate);
		wf->duty_offset_ns = wf->period_length_ns - wf->duty_length_ns;
	}

	return 0;
}

static int rp1_pwm_write_waveform(struct pwm_chip *chip,
				  struct pwm_device *pwm,
				  const void *_wfhw)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	const struct rp1_pwm_waveform *wfhw = _wfhw;
	u32 value, ctrl;

	/* set polarity */
	regmap_read(rp1->regmap, RP1_PWM_CHAN_CTRL(pwm->hwpwm), &value);
	if (!wfhw->inverted_polarity)
		value &= ~RP1_PWM_CHAN_CTRL_POLARITY;
	else
		value |= RP1_PWM_CHAN_CTRL_POLARITY;
	regmap_write(rp1->regmap, RP1_PWM_CHAN_CTRL(pwm->hwpwm), value);

	/* early exit if disabled */
	regmap_read(rp1->regmap, RP1_PWM_GLB_CTRL, &ctrl);
	if (!wfhw->enabled) {
		ctrl &= ~RP1_PWM_GLB_CTRL_CHANNEL_ENABLE(pwm->hwpwm);
		goto exit_disable;
	}

	/* set period and duty cycle */
	regmap_write(rp1->regmap,
		     RP1_PWM_RANGE(pwm->hwpwm), wfhw->period_ticks);
	regmap_write(rp1->regmap,
		     RP1_PWM_DUTY(pwm->hwpwm), wfhw->duty_ticks);

	/* enable the channel */
	ctrl |= RP1_PWM_GLB_CTRL_CHANNEL_ENABLE(pwm->hwpwm);
exit_disable:
	regmap_write(rp1->regmap, RP1_PWM_GLB_CTRL, ctrl);

	rp1_pwm_apply_config(chip, pwm);

	return 0;
}

static int rp1_pwm_read_waveform(struct pwm_chip *chip,
				 struct pwm_device *pwm,
				 void *_wfhw)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	struct rp1_pwm_waveform *wfhw = _wfhw;
	u32 value;

	regmap_read(rp1->regmap, RP1_PWM_GLB_CTRL, &value);
	wfhw->enabled = !!(value & RP1_PWM_GLB_CTRL_CHANNEL_ENABLE(pwm->hwpwm));

	regmap_read(rp1->regmap, RP1_PWM_CHAN_CTRL(pwm->hwpwm), &value);
	wfhw->inverted_polarity = !!(value & RP1_PWM_CHAN_CTRL_POLARITY);

	if (wfhw->enabled) {
		regmap_read(rp1->regmap, RP1_PWM_RANGE(pwm->hwpwm), &wfhw->period_ticks);
		regmap_read(rp1->regmap, RP1_PWM_DUTY(pwm->hwpwm), &wfhw->duty_ticks);
	} else {
		wfhw->period_ticks = 0;
		wfhw->duty_ticks = 0;
	}

	return 0;
}

static const struct pwm_ops rp1_pwm_ops = {
	.sizeof_wfhw = sizeof(struct rp1_pwm_waveform),
	.request = rp1_pwm_request,
	.round_waveform_tohw = rp1_pwm_round_waveform_tohw,
	.round_waveform_fromhw = rp1_pwm_round_waveform_fromhw,
	.read_waveform = rp1_pwm_read_waveform,
	.write_waveform = rp1_pwm_write_waveform,
};

static int rp1_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	unsigned long clk_rate;
	struct pwm_chip *chip;
	void __iomem	*base;
	struct rp1_pwm *rp1;
	int ret;

	chip = devm_pwmchip_alloc(dev, RP1_PWM_NUM_PWMS, sizeof(*rp1));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	rp1 = pwmchip_get_drvdata(chip);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	rp1->regmap = devm_regmap_init_mmio(dev, base, &rp1_pwm_regmap_config);
	if (IS_ERR(rp1->regmap))
		return dev_err_probe(dev, PTR_ERR(rp1->regmap), "Cannot initialize regmap\n");

	rp1->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(rp1->clk))
		return dev_err_probe(dev, PTR_ERR(rp1->clk), "Clock not found\n");

	ret = clk_prepare_enable(rp1->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clock\n");
	rp1->clk_enabled = true;

	ret = devm_clk_rate_exclusive_get(dev, rp1->clk);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to get exclusive rate\n");
		goto err_disable_clk;
	}

	clk_rate = clk_get_rate(rp1->clk);
	if (!clk_rate) {
		ret = dev_err_probe(dev, -EINVAL, "Failed to get clock rate\n");
		goto err_disable_clk;
	}
	/*
	 * To prevent u64 overflow in period calculations:
	 * mul_u64_u64_div_u64(period_ns, clk_rate, NSEC_PER_SEC)
	 * If clk_rate > 1 GHz, the result can overflow.
	 */
	if (clk_rate > HZ_PER_GHZ) {
		ret = dev_err_probe(dev, -EINVAL, "Clock rate > 1 GHz is not supported\n");
		goto err_disable_clk;
	}
	rp1->clk_rate = clk_rate;

	chip->ops = &rp1_pwm_ops;
	chip->atomic = true;

	platform_set_drvdata(pdev, chip);

	ret = pwmchip_add(chip);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to register PWM chip\n");
		goto err_disable_clk;
	}

	ret = of_syscon_register_regmap(np, rp1->regmap);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to register syscon\n");
		goto err_remove_chip;
	}

	return 0;

err_remove_chip:
	pwmchip_remove(chip);
err_disable_clk:
	clk_disable_unprepare(rp1->clk);

	return ret;
}

static void rp1_pwm_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);

	pwmchip_remove(chip);

	if (rp1->clk_enabled) {
		clk_disable_unprepare(rp1->clk);
		rp1->clk_enabled = false;
	}
}

static int rp1_pwm_suspend(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);

	if (rp1->clk_enabled) {
		clk_disable_unprepare(rp1->clk);
		rp1->clk_enabled = false;
	}

	return 0;
}

static int rp1_pwm_resume(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	int ret;

	ret = clk_prepare_enable(rp1->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock on resume: %pe\n", ERR_PTR(ret));
		return ret;
	}

	rp1->clk_enabled = true;

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rp1_pwm_pm_ops, rp1_pwm_suspend, rp1_pwm_resume);

static const struct of_device_id rp1_pwm_of_match[] = {
	{ .compatible = "raspberrypi,rp1-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rp1_pwm_of_match);

static struct platform_driver rp1_pwm_driver = {
	.probe = rp1_pwm_probe,
	.remove = rp1_pwm_remove,
	.driver = {
		.name = "rp1-pwm",
		.of_match_table = rp1_pwm_of_match,
		.pm = pm_ptr(&rp1_pwm_pm_ops),
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp1_pwm_driver);

MODULE_DESCRIPTION("RP1 PWM driver");
MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com>");
MODULE_AUTHOR("Andrea della Porta <andrea.porta@suse.com>");
MODULE_LICENSE("GPL");
