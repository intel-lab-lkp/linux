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
 * - Channels can be enabled/disabled and their duty cycle and period can
 *   be updated glitchlessly. Update are synchronized with the next strobe
 *   at the end of the current period of the respective channel, once the
 *   update bit is set. The update flag is global, not per-channel.
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

#define RP1_PWM_GLOBAL_CTRL	0x000
#define RP1_PWM_CHANNEL_CTRL(x)	(0x014 + ((x) * 0x10))
#define RP1_PWM_RANGE(x)	(0x018 + ((x) * 0x10))
#define RP1_PWM_PHASE(x)	(0x01C + ((x) * 0x10))
#define RP1_PWM_DUTY(x)		(0x020 + ((x) * 0x10))

/* 8:FIFO_POP_MASK + 0:Trailing edge M/S modulation */
#define RP1_PWM_CHANNEL_DEFAULT		(BIT(8) + BIT(0))
#define RP1_PWM_CHANNEL_ENABLE(x)	BIT(x)
#define RP1_PWM_POLARITY		BIT(3)
#define RP1_PWM_SET_UPDATE		BIT(31)
#define RP1_PWM_MODE_MASK		GENMASK(1, 0)

#define RP1_PWM_NUM_PWMS	4

struct rp1_pwm {
	struct regmap	*regmap;
	struct clk	*clk;
	unsigned long	clk_rate;
	bool		clk_enabled;
};

struct rp1_pwm_waveform {
	u32	period_ticks;
	u32	duty_ticks;
	bool	enabled;
	bool	inverted_polarity;
};

static const struct regmap_config rp1_pwm_regmap_config = {
	.reg_bits    = 32,
	.val_bits    = 32,
	.reg_stride  = 4,
	.max_register = 0x60,
};

static void rp1_pwm_apply_config(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	u32 value;

	/* update the changed registers on the next strobe to avoid glitches */
	regmap_read(rp1->regmap, RP1_PWM_GLOBAL_CTRL, &value);
	value |= RP1_PWM_SET_UPDATE;
	regmap_write(rp1->regmap, RP1_PWM_GLOBAL_CTRL, value);
}

static int rp1_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);

	/* init channel to reset defaults */
	regmap_write(rp1->regmap, RP1_PWM_CHANNEL_CTRL(pwm->hwpwm), RP1_PWM_CHANNEL_DEFAULT);
	return 0;
}

static int rp1_pwm_round_waveform_tohw(struct pwm_chip *chip,
				       struct pwm_device *pwm,
				       const struct pwm_waveform *wf,
				       void *_wfhw)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	struct rp1_pwm_waveform *wfhw = _wfhw;
	u64 clk_rate = rp1->clk_rate;
	u64 ticks;

	ticks = mul_u64_u64_div_u64(wf->period_length_ns, clk_rate, NSEC_PER_SEC);

	if (ticks > U32_MAX)
		ticks = U32_MAX;
	wfhw->period_ticks = ticks;

	if (wf->duty_offset_ns + wf->duty_length_ns >= wf->period_length_ns) {
		ticks = mul_u64_u64_div_u64(wf->period_length_ns - wf->duty_length_ns,
					    clk_rate, NSEC_PER_SEC);
		wfhw->inverted_polarity = true;
	} else {
		ticks = mul_u64_u64_div_u64(wf->duty_length_ns, clk_rate, NSEC_PER_SEC);
		wfhw->inverted_polarity = false;
	}

	if (ticks > wfhw->period_ticks)
		ticks = wfhw->period_ticks;
	wfhw->duty_ticks = ticks;

	wfhw->enabled = !!wfhw->duty_ticks;

	return 0;
}

static int rp1_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const void *_wfhw,
					 struct pwm_waveform *wf)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	const struct rp1_pwm_waveform *wfhw = _wfhw;
	u64 clk_rate = rp1->clk_rate;
	u32 ticks;

	memset(wf, 0, sizeof(*wf));

	if (!wfhw->enabled)
		return 0;

	wf->period_length_ns = DIV_ROUND_UP_ULL((u64)wfhw->period_ticks * NSEC_PER_SEC, clk_rate);

	if (wfhw->inverted_polarity) {
		wf->duty_length_ns = DIV_ROUND_UP_ULL((u64)wfhw->duty_ticks * NSEC_PER_SEC,
						      clk_rate);
	} else {
		wf->duty_offset_ns = DIV_ROUND_UP_ULL((u64)wfhw->duty_ticks * NSEC_PER_SEC,
						      clk_rate);
		ticks = wfhw->period_ticks - wfhw->duty_ticks;
		wf->duty_length_ns = DIV_ROUND_UP_ULL((u64)ticks * NSEC_PER_SEC, clk_rate);
	}

	return 0;
}

static int rp1_pwm_write_waveform(struct pwm_chip *chip,
				  struct pwm_device *pwm,
				  const void *_wfhw)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	const struct rp1_pwm_waveform *wfhw = _wfhw;
	u32 value;

	/* set period and duty cycle */
	regmap_write(rp1->regmap,
		     RP1_PWM_RANGE(pwm->hwpwm), wfhw->period_ticks);
	regmap_write(rp1->regmap,
		     RP1_PWM_DUTY(pwm->hwpwm), wfhw->duty_ticks);

	/* set polarity */
	regmap_read(rp1->regmap, RP1_PWM_CHANNEL_CTRL(pwm->hwpwm), &value);
	if (!wfhw->inverted_polarity)
		value &= ~RP1_PWM_POLARITY;
	else
		value |= RP1_PWM_POLARITY;
	regmap_write(rp1->regmap, RP1_PWM_CHANNEL_CTRL(pwm->hwpwm), value);

	/* enable/disable */
	regmap_read(rp1->regmap, RP1_PWM_GLOBAL_CTRL, &value);
	if (wfhw->enabled)
		value |= RP1_PWM_CHANNEL_ENABLE(pwm->hwpwm);
	else
		value &= ~RP1_PWM_CHANNEL_ENABLE(pwm->hwpwm);
	regmap_write(rp1->regmap, RP1_PWM_GLOBAL_CTRL, value);

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

	regmap_read(rp1->regmap, RP1_PWM_GLOBAL_CTRL, &value);
	wfhw->enabled = !!(value & RP1_PWM_CHANNEL_ENABLE(pwm->hwpwm));

	regmap_read(rp1->regmap, RP1_PWM_CHANNEL_CTRL(pwm->hwpwm), &value);
	wfhw->inverted_polarity = !!(value & RP1_PWM_POLARITY);

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

	ret = of_syscon_register_regmap(np, rp1->regmap);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register syscon\n");

	rp1->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(rp1->clk))
		return dev_err_probe(dev, PTR_ERR(rp1->clk), "Clock not found\n");

	ret = clk_prepare_enable(rp1->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clock\n");
	rp1->clk_enabled = true;

	ret = devm_clk_rate_exclusive_get(dev, rp1->clk);
	if (ret) {
		dev_err_probe(dev, ret, "Fail to get exclusive rate\n");
		goto err_disable_clk;
	}

	clk_rate = clk_get_rate(rp1->clk);
	if (!clk_rate) {
		ret = dev_err_probe(dev, -EINVAL, "Failed to get clock rate\n");
		goto err_disable_clk;
	}
	rp1->clk_rate = clk_rate;

	chip->ops = &rp1_pwm_ops;

	platform_set_drvdata(pdev, chip);

	ret = devm_pwmchip_add(dev, chip);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to register PWM chip\n");
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(rp1->clk);

	return ret;
}

static int rp1_pwm_suspend(struct device *dev)
{
	struct rp1_pwm *rp1 = dev_get_drvdata(dev);

	if (rp1->clk_enabled) {
		clk_disable_unprepare(rp1->clk);
		rp1->clk_enabled = false;
	}

	return 0;
}

static int rp1_pwm_resume(struct device *dev)
{
	struct rp1_pwm *rp1 = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(rp1->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock on resume: %d\n", ret);
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
	.driver = {
		.name = "rp1-pwm",
		.of_match_table = rp1_pwm_of_match,
		.pm = pm_ptr(&rp1_pwm_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(rp1_pwm_driver);

MODULE_DESCRIPTION("RP1 PWM driver");
MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com>");
MODULE_AUTHOR("Andrea della Porta <andrea.porta@suse.com>");
MODULE_LICENSE("GPL");
