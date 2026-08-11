// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2026 Axiado Corporation.
 */

/*
 * Limitations:
 * - Only normal polarity is supported.
 * - Configuration changes take effect immediately; the current period is
 *   not guaranteed to complete.
 * - Disable operations take effect immediately; the current period is not
 *   guaranteed to complete.
 * - When disabled, the output remains high.
 * - The supported period range is 2 through 0xfffffffe PWM input clock
 *   cycles; 0xffffffff is reserved by the hardware for a constant-low
 *   output. Longer periods are rounded down to the maximum.
 * - The hardware interprets a zero high time as a constant high output, so
 *   a 0% duty cycle is programmed as a constant-low period instead. That
 *   encoding does not hold the requested period, so the period cannot be
 *   read back from the hardware while the duty cycle is 0%.
 * - A 100% duty cycle is supported and produces a constant high output.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

/* Register offsets */
#define AXIADO_PWM_CTRL_REG	0x0000
#define AXIADO_PWM_PERIOD_REG	0x0004
#define AXIADO_PWM_HIGH_REG	0x0008

/* Period and duty cycle limits */
#define AXIADO_PWM_PERIOD_MIN	2
#define AXIADO_PWM_PERIOD_MAX	0xfffffffe
#define AXIADO_PWM_PERIOD_CONST_LOW	0xffffffff
#define AXIADO_PWM_DUTY_MIN	1

/* Control register bits */
#define AXIADO_PWM_CTRL_ENABLE	BIT(0)

struct axiado_pwm_chip {
	void __iomem *base;
	unsigned long rate;
	u32 cached_period;
};

struct axiado_pwm_waveform {
	u32 period;
	u32 duty;
	bool enabled;
};

static int
axiado_pwm_round_waveform_tohw(struct pwm_chip *chip,
			       struct pwm_device *pwm,
			       const struct pwm_waveform *wf,
			       void *_wfhw)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	struct axiado_pwm_waveform *wfhw = _wfhw;
	u64 period;
	u64 duty;
	int ret = 0;

	/* Encode a disabled request as a zeroed hardware waveform. */
	if (!wf->period_length_ns) {
		*wfhw = (struct axiado_pwm_waveform) {};

		return 0;
	}

	/* Only an edge-aligned waveform starting at offset zero is supported. */
	if (wf->duty_offset_ns)
		return -EINVAL;

	if (wf->duty_length_ns > wf->period_length_ns)
		return -EINVAL;

	period = mul_u64_u64_div_u64(wf->period_length_ns, axpwm->rate,
				     NSEC_PER_SEC);

	if (period < AXIADO_PWM_PERIOD_MIN) {
		period = AXIADO_PWM_PERIOD_MIN;
		ret = 1;
	} else if (period > AXIADO_PWM_PERIOD_MAX) {
		period = AXIADO_PWM_PERIOD_MAX;
	}

	/*
	 * Keep the rounded period for a 0% duty cycle. .write_waveform()
	 * translates it to the hardware constant-low representation.
	 */
	if (!wf->duty_length_ns) {
		*wfhw = (struct axiado_pwm_waveform) {
			.period = period,
			.duty = 0,
			.enabled = true,
		};

		return ret;
	}

	duty = mul_u64_u64_div_u64(wf->duty_length_ns, axpwm->rate,
				   NSEC_PER_SEC);

	/*
	 * Preserve an exact 100% duty request when the hardware period has
	 * been clamped.
	 */
	if (wf->duty_length_ns == wf->period_length_ns)
		duty = period;

	/*
	 * Period clamping can leave the converted duty greater than the
	 * final hardware period. In that case, clamp it to 100% duty.
	 */
	if (duty > period)
		duty = period;

	*wfhw = (struct axiado_pwm_waveform) {
		.period = period,
		.duty = duty,
		.enabled = true,
	};

	return ret;
}

static int
axiado_pwm_round_waveform_fromhw(struct pwm_chip *chip,
				 struct pwm_device *pwm,
				 const void *_wfhw,
				 struct pwm_waveform *wf)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	const struct axiado_pwm_waveform *wfhw = _wfhw;

	if (!wfhw->enabled) {
		*wf = (struct pwm_waveform) {
			.period_length_ns = 0,
			.duty_length_ns = 0,
			.duty_offset_ns = 0,
		};

		return 0;
	}

	*wf = (struct pwm_waveform) {
		.period_length_ns =
			mul_u64_u64_div_u64_roundup(wfhw->period, NSEC_PER_SEC,
						    axpwm->rate),
		.duty_length_ns =
			mul_u64_u64_div_u64_roundup(wfhw->duty, NSEC_PER_SEC,
						    axpwm->rate),
		.duty_offset_ns = 0,
	};

	return 0;
}

static int axiado_pwm_read_waveform(struct pwm_chip *chip,
				    struct pwm_device *pwm,
				    void *_wfhw)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	struct axiado_pwm_waveform *wfhw = _wfhw;
	u32 period;
	u32 duty;
	u32 ctrl;

	ctrl = readl(axpwm->base + AXIADO_PWM_CTRL_REG);
	period = readl(axpwm->base + AXIADO_PWM_PERIOD_REG);
	duty = readl(axpwm->base + AXIADO_PWM_HIGH_REG);

	/* The constant-low encoding doesn't hold the period, so restore it. */
	if (period == AXIADO_PWM_PERIOD_CONST_LOW) {
		period = axpwm->cached_period;
		duty = 0;
	} else if (duty > period) {
		duty = period;
	}

	*wfhw = (struct axiado_pwm_waveform) {
		.period = period,
		.duty = duty,
		.enabled = !!(ctrl & AXIADO_PWM_CTRL_ENABLE),
	};

	return 0;
}

static int axiado_pwm_write_waveform(struct pwm_chip *chip,
				     struct pwm_device *pwm,
				     const void *_wfhw)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	const struct axiado_pwm_waveform *wfhw = _wfhw;
	u32 period = wfhw->period;
	u32 duty = wfhw->duty;

	if (!wfhw->enabled) {
		writel(0, axpwm->base + AXIADO_PWM_CTRL_REG);
		return 0;
	}

	/*
	 * A zero high time produces a constant high output, so use the
	 * constant-low period encoding for a 0% duty cycle. Keep the high time
	 * non-zero because a zero value takes precedence over that encoding.
	 * Cache the requested period for .read_waveform().
	 */
	if (!duty) {
		axpwm->cached_period = period;
		period = AXIADO_PWM_PERIOD_CONST_LOW;
		duty = AXIADO_PWM_DUTY_MIN;
	}

	/*
	 * The hardware has no shadow registers. These writes may alter the
	 * active waveform before the current period has completed.
	 */
	writel(period, axpwm->base + AXIADO_PWM_PERIOD_REG);
	writel(duty, axpwm->base + AXIADO_PWM_HIGH_REG);
	writel(AXIADO_PWM_CTRL_ENABLE, axpwm->base + AXIADO_PWM_CTRL_REG);

	return 0;
}

static const struct pwm_ops axiado_pwm_ops = {
	.sizeof_wfhw = sizeof(struct axiado_pwm_waveform),
	.round_waveform_tohw = axiado_pwm_round_waveform_tohw,
	.round_waveform_fromhw = axiado_pwm_round_waveform_fromhw,
	.read_waveform = axiado_pwm_read_waveform,
	.write_waveform = axiado_pwm_write_waveform,
};

static int axiado_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct axiado_pwm_chip *axpwm;
	struct pwm_chip *chip;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*axpwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	axpwm = pwmchip_get_drvdata(chip);

	axpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(axpwm->base))
		return dev_err_probe(dev, PTR_ERR(axpwm->base),
				     "Failed to map registers\n");

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get/enable clock\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to lock clock rate\n");

	axpwm->rate = clk_get_rate(clk);
	if (!axpwm->rate)
		return dev_err_probe(dev, -EINVAL,
				     "Failed to get clock rate\n");

	/*
	 * Provide a valid fallback period if the bootloader left the
	 * constant-low encoding programmed.
	 */
	axpwm->cached_period = AXIADO_PWM_PERIOD_MIN;

	chip->ops = &axiado_pwm_ops;
	chip->atomic = true;

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add PWM chip\n");

	return 0;
}

static const struct of_device_id axiado_pwm_match[] = {
	{ .compatible = "axiado,ax3000-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, axiado_pwm_match);

static struct platform_driver axiado_pwm_driver = {
	.driver = {
		.name =  "axiado-pwm",
		.of_match_table = axiado_pwm_match,
	},
	.probe = axiado_pwm_probe,
};
module_platform_driver(axiado_pwm_driver);

MODULE_AUTHOR("Axiado Corporation");
MODULE_DESCRIPTION("Axiado PWM driver");
MODULE_LICENSE("GPL");
