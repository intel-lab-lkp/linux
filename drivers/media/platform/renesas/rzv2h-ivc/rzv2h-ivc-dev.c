// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H Input Video Control Block driver
 *
 * Copyright (C) 2024 Ideas on Board Oy
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/reset.h>

#include "rzv2h-ivc.h"

inline void rzv2h_ivc_write(struct rzv2h_ivc *ivc, u32 addr, u32 val)
{
	writel(val, ivc->base + addr);
}

void rzv2h_ivc_update_bits(struct rzv2h_ivc *ivc, unsigned int addr,
			   u32 mask, u32 val)
{
	u32 orig, new;

	orig = readl(ivc->base + addr);

	new = orig & ~mask;
	new |= val & mask;

	if (new != orig)
		writel(new, ivc->base + addr);
}

static int rzv2h_ivc_get_hardware_resources(struct rzv2h_ivc *ivc,
					    struct platform_device *pdev)
{
	const char * const reset_names[RZV2H_IVC_NUM_RESETS] = {
		"presetn",
		"vin_aresetn",
		"sresetn",
	};
	const char * const clk_names[RZV2H_IVC_NUM_CLOCKS] = {
		"pclk",
		"vin_aclk",
		"sclk",
	};
	struct resource *res;
	int ret;

	ivc->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ivc->base))
		return dev_err_probe(ivc->dev, PTR_ERR(ivc->base),
				     "failed to map IO memory\n");

	for (unsigned int i; i < ARRAY_SIZE(clk_names); i++)
		ivc->clks[i].id = clk_names[i];

	ret = devm_clk_bulk_get(ivc->dev, ARRAY_SIZE(clk_names), ivc->clks);
	if (ret)
		return dev_err_probe(ivc->dev, ret, "failed to acquire clks\n");

	for (unsigned int i = 0; i < ARRAY_SIZE(reset_names); i++)
		ivc->resets[i].id = reset_names[i];

	ret = devm_reset_control_bulk_get_optional_shared(
		ivc->dev, ARRAY_SIZE(reset_names), ivc->resets);
	if (ret)
		return dev_err_probe(ivc->dev, ret, "failed to acquire resets\n");

	return 0;
}

static void rzv2h_ivc_global_config(struct rzv2h_ivc *ivc)
{
	/* Currently we only support single-exposure input */
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_PLNUM, RZV2H_IVC_ONE_EXPOSURE);

	/*
	 * Datasheet says we should disable the interrupts before changing mode
	 * to avoid spurious IFP interrupt.
	 */
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_FM_INT_EN, 0x0);

	/*
	 * RZ/V2H documentation says software controlled configuration is not
	 * supported, and currently neither is multi-context mode. That being so
	 * we just set single context sw-hw mode.
	 */
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_FM_CONTEXT,
			RZV2H_IVC_SINGLE_CONTEXT_SW_HW_CFG);

	/*
	 * We enable the frame end interrupt so that we know when we should send
	 * follow-up frames.
	 */
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_FM_INT_EN, RZV2H_IVC_VVAL_IFPE);
}

static irqreturn_t rzv2h_ivc_isr(int irq, void *context)
{
	struct device *dev = context;
	struct rzv2h_ivc *ivc = dev_get_drvdata(dev);

	guard(spinlock)(&ivc->spinlock);
	--ivc->vvalid_ifp;
	wake_up_all(&ivc->buffers.wq);

	return IRQ_HANDLED;
}

static int rzv2h_ivc_runtime_resume(struct device *dev)
{
	struct rzv2h_ivc *ivc = dev_get_drvdata(dev);
	int ret;

	ret = request_threaded_irq(ivc->irqnum, NULL, rzv2h_ivc_isr,
				   IRQF_ONESHOT, dev_driver_string(dev), dev);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(ivc->clks), ivc->clks);
	if (ret) {
		dev_err(ivc->dev, "failed to enable clocks\n");
		goto err_free_irqnum;
	}

	ret = reset_control_bulk_deassert(ARRAY_SIZE(ivc->resets), ivc->resets);
	if (ret) {
		dev_err(ivc->dev, "failed to deassert resets\n");
		goto err_disable_clks;
	}

	rzv2h_ivc_global_config(ivc);

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(ivc->clks), ivc->clks);
err_free_irqnum:
	free_irq(ivc->irqnum, dev);

	return ret;
}

static int rzv2h_ivc_runtime_suspend(struct device *dev)
{
	struct rzv2h_ivc *ivc = dev_get_drvdata(dev);

	reset_control_bulk_assert(ARRAY_SIZE(ivc->resets), ivc->resets);
	clk_bulk_disable_unprepare(ARRAY_SIZE(ivc->clks), ivc->clks);
	free_irq(ivc->irqnum, dev);

	return 0;
}

static const struct dev_pm_ops rzv2h_ivc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rzv2h_ivc_runtime_suspend, rzv2h_ivc_runtime_resume,
			   NULL)
};

static int rzv2h_ivc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzv2h_ivc *ivc;
	int ret;

	ivc = devm_kzalloc(dev, sizeof(*ivc), GFP_KERNEL);
	if (!ivc)
		return -ENOMEM;

	ivc->dev = dev;
	platform_set_drvdata(pdev, ivc);
	mutex_init(&ivc->lock);
	spin_lock_init(&ivc->spinlock);

	ret = rzv2h_ivc_get_hardware_resources(ivc, pdev);
	if (ret)
		return ret;

	ret = rzv2h_ivc_initialise_subdevice(ivc);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	spin_lock(&ivc->spinlock);
	ivc->vvalid_ifp = 2;
	spin_unlock(&ivc->spinlock);

	ivc->irqnum = platform_get_irq(pdev, 0);
	if (ivc->irqnum < 0) {
		dev_err(dev, "failed to get interrupt\n");
		goto err_pm_runtime_disable;
	}

	return 0;

err_pm_runtime_disable:
	pm_runtime_disable(dev);

	return ret;
}

static void rzv2h_ivc_remove(struct platform_device *pdev)
{
}

static const struct of_device_id rzv2h_ivc_of_match[] = {
	{ .compatible = "renesas,r9a09g057-ivc", },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, rzv2h_ivc_of_match);

static struct platform_driver rzv2h_ivc_driver = {
	.driver = {
		.name = "rzv2h-ivc",
		.of_match_table = rzv2h_ivc_of_match,
		.pm = &rzv2h_ivc_pm_ops,
	},
	.probe = rzv2h_ivc_probe,
	.remove = rzv2h_ivc_remove,
};

module_platform_driver(rzv2h_ivc_driver);

MODULE_AUTHOR("Daniel Scally <dan.scally@ideasonboard.com>");
MODULE_DESCRIPTION("Renesas RZ/V2H Input Video Control Block driver");
MODULE_LICENSE("GPL");
