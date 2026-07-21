// SPDX-License-Identifier: GPL-2.0
/*
 * DesignWare PWM Controller driver OF
 *
 * Copyright (C) 2026 SiFive, Inc.
 */

#define DEFAULT_SYMBOL_NAMESPACE "dwc_pwm_of"

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#include "pwm-dwc.h"

struct dwc_pwm_plat_data {
	bool reset_required;
};

static int dwc_pwm_plat_probe(struct platform_device *pdev)
{
	const struct dwc_pwm_plat_data *pdata;
	struct device *dev = &pdev->dev;
	struct dwc_pwm_drvdata *data;
	u32 ctrl[DWC_TIMERS_TOTAL];
	struct pwm_chip *chip;
	struct dwc_pwm *dwc;
	bool pwm_en = false;
	u32 nr_pwm, tim_id;
	unsigned int i;
	int ret;

	data = devm_kzalloc(dev, struct_size(data, chips, 1), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	chip = dwc_pwm_alloc(dev);
	if (IS_ERR(chip))
		return dev_err_probe(dev, PTR_ERR(chip),
				     "failed to alloc pwm\n");

	dwc = to_dwc_pwm(chip);

	dwc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dwc->base))
		return PTR_ERR(dwc->base);

	if (!device_property_read_u32(dev, "snps,pwm-number", &nr_pwm)) {
		if (nr_pwm > DWC_TIMERS_TOTAL)
			dev_warn(dev, "too many PWMs (%u), capping at %u\n",
				 nr_pwm, chip->npwm);
		else
			chip->npwm = nr_pwm;
	}

	dwc->bus_clk = devm_clk_get(dev, "bus");
	if (IS_ERR(dwc->bus_clk))
		return dev_err_probe(dev, PTR_ERR(dwc->bus_clk),
				     "failed to get bus clock\n");

	dwc->clk = devm_clk_get(dev, "timer");
	if (IS_ERR(dwc->clk))
		return dev_err_probe(dev, PTR_ERR(dwc->clk),
				     "failed to get timer clock\n");

	ret = devm_clk_rate_exclusive_get(dev, dwc->clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get exclusive rate\n");

	dwc->clk_rate = clk_get_rate(dwc->clk);
	if (!dwc->clk_rate)
		return dev_err_probe(dev, -EINVAL,
				     "failed to get a valid clock rate\n");

	pdata = device_get_match_data(dev);
	if (pdata && pdata->reset_required)
		dwc->rst = devm_reset_control_get_exclusive(dev, NULL);
	else
		dwc->rst = devm_reset_control_array_get_optional_exclusive(dev);

	if (IS_ERR(dwc->rst))
		return dev_err_probe(dev, PTR_ERR(dwc->rst),
				     "failed to get reset control\n");

	ret = clk_prepare_enable(dwc->bus_clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable bus clock\n");

	ret = clk_prepare_enable(dwc->clk);
	if (ret) {
		dev_err(dev, "failed to enable timer clock\n");
		goto disable_busclk;
	}

	/* Ensure the device is out of reset before accessing MMIO registers. */
	ret = reset_control_deassert(dwc->rst);
	if (ret) {
		dev_err(dev, "failed to deassert reset\n");
		goto disable_clk;
	}

	/*
	 * Check all channels to see if any channel is enabled.
	 * Read the control register of each channel and extract the enable bit
	 */
	for (i = 0; i < chip->npwm; i++) {
		ctrl[i] = dwc_pwm_readl(dwc, DWC_TIM_CTRL(i)) & DWC_TIM_CTRL_EN;
		if (ctrl[i])
			pwm_en = true;
	}

	/*
	 * Only issue a reset pulse when all channels are disabled, so a PWM
	 * channel already running (e.g. configured by firmware before Linux
	 * took over) is left undisturbed.
	 */
	if (!pwm_en) {
		ret = reset_control_reset(dwc->rst);
		if (ret) {
			dev_err(dev, "failed to reset\n");
			goto disable_clk;
		}
	}

	/* init PWM feature */
	dwc->features = 0;
	/*
	 * Support for 0% and 100% duty cycle mode was added in version 2.11a
	 * and later.
	 */
	tim_id = dwc_pwm_readl(dwc, DWC_TIMERS_COMP_VERSION);
	if (tim_id >= DWC_TIM_VERSION_ID_2_11A)
		dwc->features |= DWC_TIM_CTRL_0N100PWM_EN;

	data->chips[0] = chip;
	dev_set_drvdata(dev, data);

	/*
	 * Save hardware state to PM context before first resume.
	 * Prevents zero-initialized context from overwriting registers
	 * and losing bootloader-preset configuration.
	 */
	for (i = 0; i < chip->npwm; i++) {
		dwc->ctx[i].cnt = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT(i));
		dwc->ctx[i].cnt2 = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT2(i));
		dwc->ctx[i].ctrl = dwc_pwm_readl(dwc, DWC_TIM_CTRL(i));
	}

	/*
	 * If any PWM channel is enabled, mark device active and hold runtime PM
	 * references for each enabled channel. Otherwise, gate the clocks.
	 *
	 * When CONFIG_PM is disabled, pm_runtime callbacks are stubs that never
	 * re-enable clocks, so keep clocks on unconditionally in that case.
	 */
	if (pwm_en) {
		pm_runtime_set_active(dev);
		for (i = 0; i < chip->npwm; i++) {
			if (ctrl[i])
				pm_runtime_get_noresume(dev);
		}
	} else if (IS_ENABLED(CONFIG_PM)) {
		clk_disable_unprepare(dwc->clk);
		clk_disable_unprepare(dwc->bus_clk);
	}

	pm_runtime_enable(dev);

	ret = pwmchip_add(chip);
	if (ret) {
		dev_err(dev, "failed to add pwm chip\n");
		goto pm_disable;
	}

	return 0;

pm_disable:
	pm_runtime_disable(dev);
	if (pwm_en) {
		for (i = 0; i < chip->npwm; i++) {
			if (ctrl[i])
				pm_runtime_put_noidle(dev);
		}
		goto disable_clk;
	}
	/*
	 * When CONFIG_PM is disabled, clocks were never gated and must be
	 * disabled here.
	 */
	if (!IS_ENABLED(CONFIG_PM))
		goto disable_clk;

	return ret;

disable_clk:
	clk_disable_unprepare(dwc->clk);
disable_busclk:
	clk_disable_unprepare(dwc->bus_clk);

	return ret;
}

static void dwc_pwm_plat_remove(struct platform_device *pdev)
{
	struct dwc_pwm_drvdata *data = platform_get_drvdata(pdev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	unsigned int idx;
	u32 ctrl;

	pwmchip_remove(chip);
	pm_runtime_disable(&pdev->dev);

	if (!pm_runtime_status_suspended(&pdev->dev)) {
		for (idx = 0; idx < chip->npwm; idx++) {
			ctrl = dwc_pwm_readl(dwc, DWC_TIM_CTRL(idx));
			if (ctrl & DWC_TIM_CTRL_EN) {
				dwc_pwm_writel(dwc, ctrl & ~DWC_TIM_CTRL_EN,
					       DWC_TIM_CTRL(idx));
				pm_runtime_put_noidle(&pdev->dev);
			}
		}
		clk_disable_unprepare(dwc->clk);
		clk_disable_unprepare(dwc->bus_clk);
	}
}

static int dwc_pwm_runtime_suspend(struct device *dev)
{
	struct dwc_pwm_drvdata *data = dev_get_drvdata(dev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	unsigned int idx;

	for (idx = 0; idx < chip->npwm; idx++) {
		dwc->ctx[idx].cnt = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT(idx));
		dwc->ctx[idx].cnt2 = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT2(idx));
		dwc->ctx[idx].ctrl = dwc_pwm_readl(dwc, DWC_TIM_CTRL(idx));
	}

	clk_disable_unprepare(dwc->clk);
	clk_disable_unprepare(dwc->bus_clk);

	return 0;
}

static int dwc_pwm_runtime_resume(struct device *dev)
{
	struct dwc_pwm_drvdata *data = dev_get_drvdata(dev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	unsigned int idx;
	int ret;

	ret = clk_prepare_enable(dwc->bus_clk);
	if (ret) {
		dev_err(dev, "failed to enable bus clock: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(dwc->clk);
	if (ret) {
		dev_err(dev, "failed to enable timer clock: %d\n", ret);
		clk_disable_unprepare(dwc->bus_clk);
		return ret;
	}

	for (idx = 0; idx < chip->npwm; idx++) {
		dwc_pwm_writel(dwc, dwc->ctx[idx].cnt, DWC_TIM_LD_CNT(idx));
		dwc_pwm_writel(dwc, dwc->ctx[idx].cnt2, DWC_TIM_LD_CNT2(idx));
		dwc_pwm_writel(dwc, dwc->ctx[idx].ctrl, DWC_TIM_CTRL(idx));
	}

	return 0;
}

static const struct dev_pm_ops dwc_pwm_pm_ops = {
	RUNTIME_PM_OPS(dwc_pwm_runtime_suspend, dwc_pwm_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct dwc_pwm_plat_data pwm_eic7700_pdata = {
	.reset_required = true,
};

static const struct of_device_id dwc_pwm_dt_ids[] = {
	{ .compatible = "snps,dw-apb-timers-pwm2" },
	{ .compatible = "eswin,eic7700-pwm", .data = &pwm_eic7700_pdata },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc_pwm_dt_ids);

static struct platform_driver dwc_pwm_plat_driver = {
	.driver = {
		.name = "dwc-pwm",
		.pm = pm_ptr(&dwc_pwm_pm_ops),
		.of_match_table = dwc_pwm_dt_ids,
	},
	.probe = dwc_pwm_plat_probe,
	.remove = dwc_pwm_plat_remove,
};

module_platform_driver(dwc_pwm_plat_driver);

MODULE_ALIAS("platform:dwc-pwm-of");
MODULE_AUTHOR("Ben Dooks <ben.dooks@codethink.co.uk>");
MODULE_DESCRIPTION("DesignWare PWM Controller");
MODULE_LICENSE("GPL");
