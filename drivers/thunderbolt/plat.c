// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thunderbolt driver - NHI driver (Platform bus specific implementation)
 *
 * Author: Chunfeng Yun <chunfeng.yun@mediatek.com>
 */

#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeirq.h>
#include <linux/slab.h>

#include "nhi.h"
#include "tb.h"

static int nhi_plat_init(struct tb_nhi *nhi)
{
	/*
	 * avoid device link fail for pcie tunnel
	 */
	pm_runtime_set_active(nhi->dev);
	pm_runtime_get_noresume(nhi->dev);
	devm_pm_runtime_enable(nhi->dev);

	return 0;
}

static irqreturn_t nhi_plat_isr(int irq, void *data)
{
	struct tb_nhi *nhi = data;
	schedule_work(&nhi->interrupt_work);
	return IRQ_HANDLED;
}

static int nhi_plat_init_irq(struct tb_nhi *nhi)
{
	int ret;

	INIT_WORK(&nhi->interrupt_work, nhi_interrupt_work);

	ret = devm_request_irq(nhi->dev, nhi->irq, nhi_plat_isr, IRQF_NO_SUSPEND,
			       dev_name(nhi->dev), nhi);
	if (ret)
		return dev_err_probe(nhi->dev, ret, "request irq failed\n");

	if (nhi->wake_irq) {
		ret = dev_pm_set_dedicated_wake_irq(nhi->dev, nhi->wake_irq);
		if (ret) {
			dev_err_probe(nhi->dev, ret, "set wake irq %d failed\n", nhi->wake_irq);
			return ret;
		}
		dev_info(nhi->dev, "wake irq %d\n", nhi->wake_irq);
	}

	return 0;
}

static void nhi_plat_exit(struct tb_nhi *nhi)
{
	devm_free_irq(nhi->dev, nhi->irq, nhi);
	flush_work(&nhi->interrupt_work);
	dev_pm_clear_wake_irq(nhi->dev);

	pm_runtime_put_noidle(nhi->dev);
	pm_runtime_disable(nhi->dev);
	pm_runtime_set_suspended(nhi->dev);
}

static const struct tb_nhi_ops nhi_plat_default_ops = {
	.init = nhi_plat_init,
	.shutdown = nhi_plat_exit,
	.init_interrupts = nhi_plat_init_irq,
};

static int nhi_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tb_nhi *nhi;

	nhi = devm_kzalloc(dev, sizeof(*nhi), GFP_KERNEL);
	if (!nhi)
		return -ENOMEM;

	nhi->dev = dev;
	nhi->ops = &nhi_plat_default_ops;

	nhi->iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nhi->iobase))
		return PTR_ERR(nhi->iobase);

	nhi->irq = platform_get_irq(pdev, 0);
	if (nhi->irq < 0)
		return dev_err_probe(dev, nhi->irq, "get irq failed\n");

	/* ignore error */
	nhi->wake_irq = platform_get_irq_optional(pdev, 1);
	if (nhi->wake_irq < 0)
		nhi->wake_irq = 0;

	return nhi_probe(nhi);
}

static void nhi_plat_do_remove(struct platform_device *pdev, bool reset)
{
	struct device *dev = &pdev->dev;
	struct tb *tb = platform_get_drvdata(pdev);
	struct tb_nhi *nhi = tb->nhi;

	/*
	 * On system shutdown/reboot force a host router reset so the
	 * CM asserts DPR on connected tbt3 devices before the router tree
	 * is removed (see tb_stop()).
	 */
	if (reset)
		nhi->host_reset = true;

	pm_runtime_get_sync(dev);
	pm_runtime_dont_use_autosuspend(dev);

	tb_domain_remove(tb);
	wait_for_completion(&nhi->domain_released);
	nhi_shutdown(nhi);
}

static void nhi_plat_remove(struct platform_device *pdev)
{
	nhi_plat_do_remove(pdev, false);
}

static void nhi_plat_shutdown(struct platform_device *pdev)
{
	nhi_plat_do_remove(pdev, true);
}


#ifdef CONFIG_OF
static const struct of_device_id nhi_plat_of_match[] = {
	{ .compatible = "thunderbolt-platform" },
	{ }
};
MODULE_DEVICE_TABLE(of, nhi_plat_of_match);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id nhi_plat_acpi_match[] = {
	{ "ACPI0015", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nhi_plat_acpi_match);
#endif

static struct platform_driver nhi_plat_driver = {
	.probe = nhi_plat_probe,
	.remove = nhi_plat_remove,
	.shutdown = nhi_plat_shutdown,
	.driver = {
		.name = "thunderbolt",
		.of_match_table = of_match_ptr(nhi_plat_of_match),
		.acpi_match_table = ACPI_PTR(nhi_plat_acpi_match),
		.pm = &nhi_pm_ops,
	},
};

module_platform_driver(nhi_plat_driver);
MODULE_ALIAS("platform:thunderbolt");
MODULE_DESCRIPTION("Thunderbolt/USB4 platform host interface driver");
MODULE_LICENSE("GPL v2");
