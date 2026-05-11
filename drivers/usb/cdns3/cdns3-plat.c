// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2018-2020 Cadence.
 * Copyright (C) 2017-2018 NXP
 * Copyright (C) 2019 Texas Instruments
 *
 *
 * Author: Peter Chen <peter.chen@nxp.com>
 *         Pawel Laszczak <pawell@cadence.com>
 *         Roger Quadros <rogerq@ti.com>
 */

#include <linux/module.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "core.h"
#include "gadget-export.h"
#include "glue.h"
#include "host-export.h"
#include "drd.h"

static int set_phy_power_on(struct cdns *cdns)
{
	int ret;

	ret = phy_power_on(cdns->usb2_phy);
	if (ret)
		return ret;

	ret = phy_power_on(cdns->usb3_phy);
	if (ret)
		phy_power_off(cdns->usb2_phy);

	return ret;
}

static void set_phy_power_off(struct cdns *cdns)
{
	phy_power_off(cdns->usb3_phy);
	phy_power_off(cdns->usb2_phy);
}

static int cdns3_plat_gadget_init(struct cdns *cdns)
{
	if (cdns->version < CDNSP_CONTROLLER_V2)
		return cdns3_gadget_init(cdns);
	else
		return cdnsp_gadget_init(cdns);
}

static int cdns3_plat_host_init(struct cdns *cdns)
{
	return cdns_host_init(cdns);
}

/**
 * cdns3_core_probe - Initialize the Cadence USB3 platform core
 * @data: Controller context and platform device supplied by the glue layer
 *
 * Returns 0 on success otherwise negative errno
 */
int cdns3_core_probe(const struct cdns3_probe_data *data)
{
	struct platform_device *pdev = data->pdev;
	struct device *dev = &pdev->dev;
	struct cdns *cdns = data->cdns;
	struct resource *res;
	void __iomem *regs;
	int ret;

	dev_set_drvdata(dev, cdns);

	ret = platform_get_irq_byname(pdev, "host");
	if (ret < 0)
		return ret;

	cdns->xhci_res[0].start = ret;
	cdns->xhci_res[0].end = ret;
	cdns->xhci_res[0].flags = IORESOURCE_IRQ | irq_get_trigger_type(ret);
	cdns->xhci_res[0].name = "host";

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "xhci");
	if (!res) {
		dev_err(dev, "couldn't get xhci resource\n");
		return -ENXIO;
	}

	cdns->xhci_res[1] = *res;

	cdns->dev_irq = platform_get_irq_byname(pdev, "peripheral");

	if (cdns->dev_irq < 0)
		return dev_err_probe(dev, cdns->dev_irq,
				     "Failed to get peripheral IRQ\n");

	regs = devm_platform_ioremap_resource_byname(pdev, "dev");
	if (IS_ERR(regs))
		return dev_err_probe(dev, PTR_ERR(regs),
				     "Failed to get dev base\n");

	cdns->dev_regs	= regs;

	cdns->otg_irq = platform_get_irq_byname(pdev, "otg");
	if (cdns->otg_irq < 0)
		return dev_err_probe(dev, cdns->otg_irq,
				     "Failed to get otg IRQ\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "otg");
	if (!res) {
		dev_err(dev, "couldn't get otg resource\n");
		return -ENXIO;
	}

	cdns->phyrst_a_enable = device_property_read_bool(dev, "cdns,phyrst-a-enable");

	cdns->otg_res = *res;

	cdns->wakeup_irq = platform_get_irq_byname_optional(pdev, "wakeup");
	if (cdns->wakeup_irq == -EPROBE_DEFER)
		return cdns->wakeup_irq;

	if (cdns->wakeup_irq < 0) {
		dev_dbg(dev, "couldn't get wakeup irq\n");
		cdns->wakeup_irq = 0x0;
	}

	cdns->usb2_phy = devm_phy_optional_get(dev, "cdns3,usb2-phy");
	if (IS_ERR(cdns->usb2_phy))
		return dev_err_probe(dev, PTR_ERR(cdns->usb2_phy),
				     "Failed to get cdn3,usb2-phy\n");

	ret = phy_init(cdns->usb2_phy);
	if (ret)
		return ret;

	cdns->usb3_phy = devm_phy_optional_get(dev, "cdns3,usb3-phy");
	if (IS_ERR(cdns->usb3_phy))
		return dev_err_probe(dev, PTR_ERR(cdns->usb3_phy),
				     "Failed to get cdn3,usb3-phy\n");

	ret = phy_init(cdns->usb3_phy);
	if (ret)
		goto err_phy3_init;

	ret = set_phy_power_on(cdns);
	if (ret)
		goto err_phy_power_on;

	ret = cdns_init(cdns);
	if (ret)
		goto err_cdns_init;

	cdns->gadget_init = cdns3_plat_gadget_init;
	cdns->host_init = cdns3_plat_host_init;
	ret = cdns_core_init_role(cdns);
	if (ret)
		goto err_cdns_init;

	device_set_wakeup_capable(dev, true);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	if (!(cdns->pdata && (cdns->pdata->quirks & CDNS3_DEFAULT_PM_RUNTIME_ALLOW)))
		pm_runtime_forbid(dev);

	/*
	 * The controller needs less time between bus and controller suspend,
	 * and we also needs a small delay to avoid frequently entering low
	 * power mode.
	 */
	pm_runtime_set_autosuspend_delay(dev, 20);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_use_autosuspend(dev);

	return 0;

err_cdns_init:
	set_phy_power_off(cdns);
err_phy_power_on:
	phy_exit(cdns->usb3_phy);
err_phy3_init:
	phy_exit(cdns->usb2_phy);

	return ret;
}
EXPORT_SYMBOL_GPL(cdns3_core_probe);

/**
 * cdns3_plat_probe - probe for cdns3 core device
 * @pdev: Pointer to cdns3 core platform device
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdns *cdns;
	struct cdns3_probe_data probe_data;

	cdns = devm_kzalloc(dev, sizeof(*cdns), GFP_KERNEL);
	if (!cdns)
		return -ENOMEM;

	cdns->dev = dev;
	cdns->pdata = dev_get_platdata(dev);
	if (cdns->pdata && cdns->pdata->override_apb_timeout)
		cdns->override_apb_timeout = cdns->pdata->override_apb_timeout;

	probe_data.cdns = cdns;
	probe_data.pdev = pdev;

	return cdns3_core_probe(&probe_data);
}

/**
 * cdns3_core_remove - Tear down the Cadence USB3 platform core
 * @cdns: Controller context previously initialized by cdns3_core_probe()
 */
void cdns3_core_remove(struct cdns *cdns)
{
	struct device *dev = cdns->dev;

	pm_runtime_get_sync(dev);
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	cdns_remove(cdns);
	set_phy_power_off(cdns);
	phy_exit(cdns->usb2_phy);
	phy_exit(cdns->usb3_phy);
}
EXPORT_SYMBOL_GPL(cdns3_core_remove);

/**
 * cdns3_plat_remove() - unbind drd driver and clean up
 * @pdev: Pointer to Linux platform device
 */
static void cdns3_plat_remove(struct platform_device *pdev)
{
	cdns3_core_remove(platform_get_drvdata(pdev));
}

#ifdef CONFIG_PM

static int cdns3_set_platform_suspend(struct cdns *cdns, bool suspend, bool wakeup)
{
	if (cdns->pdata && cdns->pdata->platform_suspend)
		return cdns->pdata->platform_suspend(cdns->dev, suspend, wakeup);

	return 0;
}

static int cdns3_controller_suspend(struct cdns *cdns, pm_message_t msg)
{
	struct device *dev = cdns->dev;
	bool wakeup;
	unsigned long flags;

	if (cdns->in_lpm)
		return 0;

	if (PMSG_IS_AUTO(msg))
		wakeup = true;
	else
		wakeup = device_may_wakeup(dev);

	cdns3_set_platform_suspend(cdns, true, wakeup);
	set_phy_power_off(cdns);
	spin_lock_irqsave(&cdns->lock, flags);
	cdns->in_lpm = true;
	spin_unlock_irqrestore(&cdns->lock, flags);
	dev_dbg(cdns->dev, "%s ends\n", __func__);

	return 0;
}

static int cdns3_controller_resume(struct cdns *cdns, pm_message_t msg)
{
	int ret;
	unsigned long flags;

	if (!cdns->in_lpm)
		return 0;

	if (cdns_power_is_lost(cdns)) {
		phy_exit(cdns->usb2_phy);
		ret = phy_init(cdns->usb2_phy);
		if (ret)
			return ret;

		phy_exit(cdns->usb3_phy);
		ret = phy_init(cdns->usb3_phy);
		if (ret)
			return ret;
	}

	ret = set_phy_power_on(cdns);
	if (ret)
		return ret;

	cdns3_set_platform_suspend(cdns, false, false);

	spin_lock_irqsave(&cdns->lock, flags);
	cdns_resume(cdns);
	cdns->in_lpm = false;
	spin_unlock_irqrestore(&cdns->lock, flags);
	cdns_set_active(cdns, !PMSG_IS_AUTO(msg));
	if (cdns->wakeup_pending) {
		cdns->wakeup_pending = false;
		enable_irq(cdns->wakeup_irq);
	}
	dev_dbg(cdns->dev, "%s ends\n", __func__);

	return ret;
}

int cdns3_runtime_suspend(struct cdns *cdns)
{
	return cdns3_controller_suspend(cdns, PMSG_AUTO_SUSPEND);
}
EXPORT_SYMBOL_GPL(cdns3_runtime_suspend);

int cdns3_runtime_resume(struct cdns *cdns)
{
	return cdns3_controller_resume(cdns, PMSG_AUTO_RESUME);
}
EXPORT_SYMBOL_GPL(cdns3_runtime_resume);

static int cdns3_dev_runtime_suspend(struct device *dev)
{
	return cdns3_runtime_suspend(dev_get_drvdata(dev));
}

static int cdns3_dev_runtime_resume(struct device *dev)
{
	return cdns3_runtime_resume(dev_get_drvdata(dev));
}

#ifdef CONFIG_PM_SLEEP
int cdns3_pm_suspend(struct cdns *cdns)
{
	struct device *dev = cdns->dev;
	int ret;

	cdns_suspend(cdns);

	ret = cdns3_controller_suspend(cdns, PMSG_SUSPEND);
	if (ret)
		return ret;

	if (device_may_wakeup(dev) && cdns->wakeup_irq)
		enable_irq_wake(cdns->wakeup_irq);

	return ret;
}
EXPORT_SYMBOL_GPL(cdns3_pm_suspend);

int cdns3_pm_resume(struct cdns *cdns)
{
	return cdns3_controller_resume(cdns, PMSG_RESUME);
}
EXPORT_SYMBOL_GPL(cdns3_pm_resume);

static int cdns3_dev_pm_suspend(struct device *dev)
{
	return cdns3_pm_suspend(dev_get_drvdata(dev));
}

static int cdns3_dev_pm_resume(struct device *dev)
{
	return cdns3_pm_resume(dev_get_drvdata(dev));
}
#endif /* CONFIG_PM_SLEEP */
#endif /* CONFIG_PM */

static const struct dev_pm_ops cdns3_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cdns3_dev_pm_suspend, cdns3_dev_pm_resume)
	SET_RUNTIME_PM_OPS(cdns3_dev_runtime_suspend,
			   cdns3_dev_runtime_resume, NULL)
};

#ifdef CONFIG_OF
static const struct of_device_id of_cdns3_match[] = {
	{ .compatible = "cdns,usb3" },
	{ },
};
MODULE_DEVICE_TABLE(of, of_cdns3_match);
#endif

static struct platform_driver cdns3_driver = {
	.probe		= cdns3_plat_probe,
	.remove		= cdns3_plat_remove,
	.driver		= {
		.name	= "cdns-usb3",
		.of_match_table	= of_match_ptr(of_cdns3_match),
		.pm	= &cdns3_pm_ops,
	},
};

module_platform_driver(cdns3_driver);

MODULE_ALIAS("platform:cdns3");
