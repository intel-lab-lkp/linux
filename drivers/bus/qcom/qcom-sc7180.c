// SPDX-License-Identifier: GPL-2.0
/*
 * SoC bus driver for Qualcomm SC7180 SoCs
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_domain.h>

struct qcom_soc_pm_domain {
	struct clk *clk;
	struct generic_pm_domain pd;
};

static struct qcom_soc_pm_domain *
gpd_to_qcom_soc_pm_domain(struct generic_pm_domain *gpd)
{
	return container_of(gpd, struct qcom_soc_pm_domain, pd);
}

static struct qcom_soc_pm_domain *pd_to_qcom_soc_pm_domain(struct dev_pm_domain *pd)
{
	struct generic_pm_domain *gpd;

	gpd = container_of(pd, struct generic_pm_domain, domain);

	return gpd_to_qcom_soc_pm_domain(gpd);
}

static struct qcom_soc_pm_domain *dev_to_qcom_soc_pm_domain(struct device *dev)
{
	struct dev_pm_domain *pd;

	pd = dev->pm_domain;
	if (!pd)
		return NULL;

	return pd_to_qcom_soc_pm_domain(pd);
}

static struct platform_device *
qcom_soc_alloc_device(struct platform_device *socdev, const char *compatible)
{
	struct device_node *np __free(device_node);

	np = of_get_compatible_child(socdev->dev.of_node, compatible);

	return of_platform_device_alloc(np, NULL, &socdev->dev);
}

static int qcom_soc_domain_activate(struct device *dev)
{
	struct qcom_soc_pm_domain *soc_domain;

	dev_info(dev, "Activating device\n");
	soc_domain = dev_to_qcom_soc_pm_domain(dev);

	soc_domain->clk = devm_clk_get(dev, NULL);

	return PTR_ERR_OR_ZERO(soc_domain->clk);
}

static int qcom_soc_domain_power_on(struct generic_pm_domain *domain)
{
	struct qcom_soc_pm_domain *soc_domain;

	pr_info("Powering on device\n");
	soc_domain = gpd_to_qcom_soc_pm_domain(domain);

	return clk_prepare_enable(soc_domain->clk);
}

static int qcom_soc_domain_power_off(struct generic_pm_domain *domain)
{
	struct qcom_soc_pm_domain *soc_domain;

	pr_info("Powering off device\n");
	soc_domain = gpd_to_qcom_soc_pm_domain(domain);

	clk_disable_unprepare(soc_domain->clk);

	return 0;
}

static int qcom_soc_add_clk_domain(struct platform_device *socdev,
				   struct platform_device *pdev)
{
	struct qcom_soc_pm_domain *domain;
	struct generic_pm_domain *pd;
	int ret;

	domain = devm_kzalloc(&socdev->dev, sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return -ENOMEM;

	pd = &domain->pd;
	pd->name = "wdog";
	ret = pm_genpd_init(pd, NULL, false);
	if (ret)
		return ret;

	/* TODO: Wrap this in a generic_pm_domain function similar to power_on() */
	pd->domain.activate = qcom_soc_domain_activate;
	pd->power_on = qcom_soc_domain_power_on;
	pd->power_off = qcom_soc_domain_power_off;

	dev_info(&socdev->dev, "adding pm domain for %s\n", dev_name(&pdev->dev));
	dev_pm_domain_set(&pdev->dev, &pd->domain);

	return 0;
}

static int qcom_soc_sc7180_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct platform_device *sdev;
	int ret;

	sdev = qcom_soc_alloc_device(pdev, "qcom,apss-wdt-sc7180");
	if (!sdev)
		return dev_err_probe(dev, -ENODEV, "Failed to alloc sdev\n");

	ret = qcom_soc_add_clk_domain(pdev, sdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add clk domain to sdev\n");

	ret = of_platform_device_add(sdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add sdev to bus\n");

	return of_platform_populate(np, NULL, NULL, dev);
}

static const struct of_device_id qcom_soc_sc7180_match[] = {
	{ .compatible = "qcom,soc-sc7180", },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_soc_sc7180_match);

static struct platform_driver qcom_soc_sc7180_driver = {
	.probe = qcom_soc_sc7180_probe,
	.driver = {
		.name = "qcom-soc-sc7180",
		.of_match_table = qcom_soc_sc7180_match,
		.suppress_bind_attrs = true,
	},
};

static int __init qcom_soc_sc7180_driver_init(void)
{
	return platform_driver_register(&qcom_soc_sc7180_driver);
}
/* Register before simple-bus driver. */
arch_initcall(qcom_soc_sc7180_driver_init);

static void __exit qcom_soc_sc7180_driver_exit(void)
{
	platform_driver_unregister(&qcom_soc_sc7180_driver);
}
module_exit(qcom_soc_sc7180_driver_exit);

MODULE_DESCRIPTION("Qualcomm SC7180 SoC Driver");
MODULE_LICENSE("GPL");
