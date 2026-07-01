// SPDX-License-Identifier: GPL-2.0
/*
 * OpenCores PTC PWM Driver
 *
 * https://opencores.org/projects/ptc
 *
 * Copyright (C) 2018-2026 StarFive Technology Co., Ltd.
 *
 * Limitations:
 * - The hardware only supports inverted polarity.
 * - The hardware minimum period / duty_cycle of PWM is (1 / pwm_apb clock frequency).
 * - The hardware maximum period / duty_cycle of PWM is (U32_MAX / pwm_apb clock frequency).
 * - The output is immediately set to low when the module is disabled.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#define OCPWM_HRC	0x4
#define OCPWM_LRC	0x8
#define OCPWM_CTRL	0xC

#define OCPWM_CTRL_EN	BIT(0)
#define OCPWM_CTRL_OE	BIT(3)
#define OCPWM_CTRL_RST	BIT(7)

#define OCPWM_NUM_SAVED_REGS	3

struct ocores_pwm_device {
	void __iomem *base;
	struct clk *clk;
	unsigned long clk_rate;
	struct reset_control *rst;
	u32 saved_regs[OCPWM_NUM_SAVED_REGS];
};

static int ocores_pwm_get_state(struct pwm_chip *chip,
				struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct ocores_pwm_device *ddata = pwmchip_get_drvdata(chip);
	u32 period_data, duty_data, ctrl_data;
	int ret;

	ret = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (ret < 0)
		return ret;

	period_data = readl(ddata->base + OCPWM_LRC);
	duty_data = readl(ddata->base + OCPWM_HRC);
	ctrl_data = readl(ddata->base + OCPWM_CTRL);

	state->period = DIV_ROUND_UP_ULL((u64)period_data * NSEC_PER_SEC, ddata->clk_rate);
	state->duty_cycle = DIV_ROUND_UP_ULL((u64)duty_data * NSEC_PER_SEC, ddata->clk_rate);
	if (state->duty_cycle > state->period)
		state->duty_cycle = state->period;

	state->polarity = PWM_POLARITY_INVERSED;
	state->enabled = (ctrl_data & OCPWM_CTRL_EN) ? true : false;

	pm_runtime_put(pwmchip_parent(chip));

	return 0;
}

static int ocores_pwm_apply(struct pwm_chip *chip,
			    struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct ocores_pwm_device *ddata = pwmchip_get_drvdata(chip);
	bool was_enabled = pwm_is_enabled(pwm);
	u64 period_data, duty_data;
	int ret;

	if (state->polarity != PWM_POLARITY_INVERSED)
		return -EINVAL;

	if (!state->enabled) {
		if (was_enabled) {
			writel(0, ddata->base + OCPWM_CTRL);
			pm_runtime_put(pwmchip_parent(chip));
		}
		return 0;
	}

	period_data = mul_u64_u32_div(state->period, ddata->clk_rate, NSEC_PER_SEC);
	if (period_data > U32_MAX)
		period_data = U32_MAX;

	duty_data = mul_u64_u32_div(state->duty_cycle, ddata->clk_rate, NSEC_PER_SEC);
	if (duty_data > U32_MAX)
		duty_data = U32_MAX;

	if (!period_data || (!duty_data && state->duty_cycle))
		return -EINVAL;

	if (!was_enabled) {
		ret = pm_runtime_resume_and_get(pwmchip_parent(chip));
		if (ret < 0)
			return ret;
	}

	writel(0, ddata->base + OCPWM_CTRL);
	writel(OCPWM_CTRL_RST, ddata->base + OCPWM_CTRL);

	writel(period_data, ddata->base + OCPWM_LRC);
	writel(duty_data, ddata->base + OCPWM_HRC);
	writel(OCPWM_CTRL_OE | OCPWM_CTRL_EN, ddata->base + OCPWM_CTRL);

	return 0;
}

static void ocores_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ocores_pwm_device *ddata = pwmchip_get_drvdata(chip);

	if (pwm_is_enabled(pwm)) {
		writel(0, ddata->base + OCPWM_CTRL);
		pm_runtime_put_sync(pwmchip_parent(chip));
	}
}

static const struct pwm_ops ocores_pwm_ops = {
	.get_state = ocores_pwm_get_state,
	.apply = ocores_pwm_apply,
	.free = ocores_pwm_free,
};

static int ocores_pwm_runtime_suspend(struct device *dev)
{
	struct ocores_pwm_device *ddata = dev_get_drvdata(dev);

	clk_disable_unprepare(ddata->clk);

	return 0;
}

static int ocores_pwm_runtime_resume(struct device *dev)
{
	struct ocores_pwm_device *ddata = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(ddata->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clock\n");

	return 0;
}

static int __maybe_unused ocores_pwm_suspend(struct device *dev)
{
	struct ocores_pwm_device *ddata = dev_get_drvdata(dev);
	int ret, i;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < OCPWM_NUM_SAVED_REGS; i++)
		ddata->saved_regs[i] = readl(ddata->base + 4 + 4 * i);

	pm_runtime_put_sync(dev);

	return pm_runtime_force_suspend(dev);
}

static int __maybe_unused ocores_pwm_resume(struct device *dev)
{
	struct ocores_pwm_device *ddata = dev_get_drvdata(dev);
	int ret, i;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	writel(0, ddata->base + OCPWM_CTRL);
	writel(OCPWM_CTRL_RST, ddata->base + OCPWM_CTRL);
	for (i = 0; i < OCPWM_NUM_SAVED_REGS; i++)
		writel(ddata->saved_regs[i], ddata->base + 4 + 4 * i);

	pm_runtime_put_sync(dev);

	return 0;
}

static const struct dev_pm_ops ocores_pwm_pm_ops = {
	RUNTIME_PM_OPS(ocores_pwm_runtime_suspend,
		       ocores_pwm_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(ocores_pwm_suspend, ocores_pwm_resume)
};

static void ocores_pwm_pm_disable(void *data)
{
	struct device *dev = data;
	struct ocores_pwm_device *ddata = dev_get_drvdata(dev);

	pm_runtime_disable(dev);

	if (!pm_runtime_status_suspended(dev)) {
		/* Balance probe's pm_runtime_get_noresume() for bootloader-enabled PWM. */
		if (readl(ddata->base + OCPWM_CTRL) & OCPWM_CTRL_EN)
			pm_runtime_put_noidle(dev);

		ocores_pwm_runtime_suspend(dev);
	}

	reset_control_assert(ddata->rst);
}

static int ocores_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ocores_pwm_device *ddata;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*ddata));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	chip->ops = &ocores_pwm_ops;
	ddata = pwmchip_get_drvdata(chip);

	ddata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->base))
		return dev_err_probe(dev, PTR_ERR(ddata->base),
				     "Failed to map IO resources\n");

	ddata->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(ddata->clk))
		return dev_err_probe(dev, PTR_ERR(ddata->clk),
				     "Failed to get clock\n");

	ddata->clk_rate = clk_get_rate(ddata->clk);
	if (!ddata->clk_rate || ddata->clk_rate > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid clock rate: %lu\n", ddata->clk_rate);

	ddata->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(ddata->rst))
		return dev_err_probe(dev, PTR_ERR(ddata->rst),
				     "Failed to get reset\n");

	platform_set_drvdata(pdev, ddata);

	ret = ocores_pwm_runtime_resume(dev);
	if (ret)
		return ret;

	ret = reset_control_deassert(ddata->rst);
	if (ret)
		goto err_clk_disable;

	ret = pm_runtime_set_active(dev);
	if (ret)
		goto err_reset_assert;

	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	if (!(readl(ddata->base + OCPWM_CTRL) & OCPWM_CTRL_EN))
		pm_runtime_put_sync(dev);

	ret = devm_add_action_or_reset(dev, ocores_pwm_pm_disable, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add pm disable action\n");

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Could not register PWM chip\n");

	return 0;

err_reset_assert:
	reset_control_assert(ddata->rst);
err_clk_disable:
	ocores_pwm_runtime_suspend(dev);
	return dev_err_probe(dev, ret, "Failed to init pwm power\n");
}

static const struct of_device_id ocores_pwm_of_match[] = {
	{ .compatible = "opencores,pwm-v1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ocores_pwm_of_match);

static struct platform_driver ocores_pwm_driver = {
	.probe = ocores_pwm_probe,
	.driver = {
		.name = "ocores-pwm",
		.of_match_table = ocores_pwm_of_match,
		.pm = pm_ptr(&ocores_pwm_pm_ops),
	},
};
module_platform_driver(ocores_pwm_driver);

MODULE_AUTHOR("Jieqin Chen");
MODULE_AUTHOR("Hal Feng <hal.feng@starfivetech.com>");
MODULE_DESCRIPTION("OpenCores PTC PWM driver");
MODULE_LICENSE("GPL");
