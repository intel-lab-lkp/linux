// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN PWM driver (Based on DesignWare PWM Controller driver)
 *
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * Authors:
 *	Xiang Xu <xuxiang@eswincomputing.com>
 *	Guosheng Wang <wangguosheng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 *
 * Characteristics:
 * - The hardware can generate 0% and 100% duty cycle.
 *   - 0% duty cycle (fully off)
 *   - 100% duty cycle (fully on)
 *
 * Limitations:
 * - The duty cycle and period cannot both be set to 0.
 * - The controller hardware provides up to 3 PWM channels.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#include "pwm-dwc.h"

#define DW_PWMS_TOTAL	3

static int eic7700_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dwc_pwm_drvdata *data;
	struct pwm_chip *chip = NULL;
	struct dwc_pwm *dwc = NULL;
	int ret;

	data = devm_kzalloc(dev, sizeof(data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	chip = dwc_pwm_alloc(dev);
	if (IS_ERR(chip))
		return dev_err_probe(dev, -ENOMEM, "failed to alloc pwm\n");

	dwc = to_dwc_pwm(chip);

	if (of_property_read_bool(dev->of_node, "snps,pwm-full-range-enable"))
		dwc->pwm_0n100_enable = true;

	dwc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dwc->base))
		return dev_err_probe(dev, PTR_ERR(dwc->base),
				     "failed to get base\n");

	dwc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dwc->clk))
		return dev_err_probe(dev, PTR_ERR(dwc->clk),
				     "failed to get clock\n");

	ret = clk_prepare_enable(dwc->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clock\n");

	dwc->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(dwc->rst)) {
		dev_err(dev, "failed to get reset control\n");
		ret = PTR_ERR(dwc->rst);
		goto err_clk_disable;
	}

	ret = reset_control_deassert(dwc->rst);
	if (ret) {
		dev_err(dev, "failed to deassert reset: %d\n", ret);
		goto err_clk_disable;
	}

	ret = devm_pwmchip_add(dev, chip);
	if (ret) {
		dev_err(dev, "failed to add pwmchip: %d\n", ret);
		goto err_reset_assert;
	}

	data->chips[0] = chip;
	dev_set_drvdata(dev, data);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_get_noresume(dev);

	return 0;

err_reset_assert:
	reset_control_assert(dwc->rst);
err_clk_disable:
	clk_disable_unprepare(dwc->clk);

	return ret;
}

static void eic7700_pwm_remove(struct platform_device *pdev)
{
	struct dwc_pwm_drvdata *data = platform_get_drvdata(pdev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	struct device *dev = &pdev->dev;

	clk_disable_unprepare(dwc->clk);
	reset_control_assert(dwc->rst);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

static int eic7700_pwm_runtime_suspend(struct device *dev)
{
	struct dwc_pwm_drvdata *data = dev_get_drvdata(dev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	int idx, ret;

	for (idx = 0; idx < DW_PWMS_TOTAL; idx++) {
		if (chip->pwms[idx].state.enabled)
			return -EBUSY;

		dwc->ctx[idx].cnt = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT(idx));
		dwc->ctx[idx].cnt2 = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT2(idx));
		dwc->ctx[idx].ctrl = dwc_pwm_readl(dwc, DWC_TIM_CTRL(idx));
	}

	clk_disable_unprepare(dwc->clk);
	ret = pinctrl_pm_select_sleep_state(dev);
	if (ret) {
		dev_err(dev, "failed to select sleep state: %d\n", ret);
		clk_prepare_enable(dwc->clk);
		return ret;
	}

	return 0;
}

static int eic7700_pwm_runtime_resume(struct device *dev)
{
	struct dwc_pwm_drvdata *data = dev_get_drvdata(dev);
	struct pwm_chip *chip = data->chips[0];
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	int idx, ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret) {
		dev_err(dev, "failed to select default state: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(dwc->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		return ret;
	}

	for (idx = 0; idx < DW_PWMS_TOTAL; idx++) {
		dwc_pwm_writel(dwc, dwc->ctx[idx].cnt, DWC_TIM_LD_CNT(idx));
		dwc_pwm_writel(dwc, dwc->ctx[idx].cnt2, DWC_TIM_LD_CNT2(idx));
		dwc_pwm_writel(dwc, dwc->ctx[idx].ctrl, DWC_TIM_CTRL(idx));
	}

	return 0;
}

static int eic7700_pwm_suspend(struct device *dev)
{
	int ret;

	if (!pm_runtime_suspended(dev)) {
		ret = eic7700_pwm_runtime_suspend(dev);
		if (ret)
			return ret;
	}

	return 0;
}

static int eic7700_pwm_resume(struct device *dev)
{
	int ret;

	if (!pm_runtime_suspended(dev)) {
		ret = eic7700_pwm_runtime_resume(dev);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct dev_pm_ops eic7700_pwm_pm_ops = {
	RUNTIME_PM_OPS(eic7700_pwm_runtime_suspend, eic7700_pwm_runtime_resume,
		       NULL)
	SYSTEM_SLEEP_PM_OPS(eic7700_pwm_suspend, eic7700_pwm_resume)
};

static const struct of_device_id eic7700_pwm_id_table[] = {
	{ .compatible = "eswin,eic7700-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, eic7700_pwm_id_table);

static struct platform_driver eic7700_pwm_driver = {
	.probe = eic7700_pwm_probe,
	.remove = eic7700_pwm_remove,
	.driver = {
		.name	= "eic7700-pwm",
		.pm = pm_sleep_ptr(&eic7700_pwm_pm_ops),
		.of_match_table = of_match_ptr(eic7700_pwm_id_table),
	},
};

module_platform_driver(eic7700_pwm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xiang Xu <xuxiang@eswincomputing.com>");
MODULE_AUTHOR("Guosheng Wang <wangguosheng@eswincomputing.com>");
MODULE_AUTHOR("Xuyang Dong <dongxuyang@eswincomputing.com>");
MODULE_DESCRIPTION("ESWIN EIC7700 PWM Controller");
